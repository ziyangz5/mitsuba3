#include <mitsuba/render/denoiser.h>
#include <mitsuba/render/optix_api.h>
#include <drjit-core/optix.h>

NAMESPACE_BEGIN(mitsuba)

template <typename Float, typename Spectrum>
static OptixImage2D optixImage2DfromTensor(
    const typename Denoiser<Float, Spectrum>::TensorXf &tensor,
    OptixPixelFormat pixel_format) {
    return { (CUdeviceptr) tensor.data(),
             (unsigned int) tensor.shape(1),
             (unsigned int) tensor.shape(0),
             (unsigned int) (tensor.shape(1) * tensor.shape(2) * sizeof(float)),
             (unsigned int) (tensor.shape(2) * sizeof(float)),
             pixel_format };
}

MI_VARIANT Denoiser<Float, Spectrum>::Denoiser(const ScalarVector2u &input_size,
                                               bool albedo, bool normals,
                                               bool temporal)
    : m_temporal(temporal) {
    if (normals && !albedo)
        Throw("The denoiser cannot use normals to guide its process without "
              "also providing albedo information!");

    optix_initialize();

    OptixDeviceContext context = jit_optix_context();
    m_denoiser = nullptr;
    m_options = {};
    m_options.guideAlbedo = albedo;
    m_options.guideNormal = normals;
    OptixDenoiserModelKind model_kind = temporal
                                            ? OPTIX_DENOISER_MODEL_KIND_TEMPORAL
                                            : OPTIX_DENOISER_MODEL_KIND_HDR;
    jit_optix_check(
        optixDenoiserCreate(context, model_kind, &m_options, &m_denoiser));

    OptixDenoiserSizes sizes = {};
    jit_optix_check(optixDenoiserComputeMemoryResources(
        m_denoiser, input_size.x(), input_size.y(), &sizes));
    CUstream stream = jit_cuda_stream();
    m_state_size = sizes.stateSizeInBytes;
    m_state = jit_malloc(AllocType::Device, m_state_size);
    m_scratch_size = sizes.withoutOverlapScratchSizeInBytes;
    m_scratch = jit_malloc(AllocType::Device, m_scratch_size);
    jit_optix_check(optixDenoiserSetup(m_denoiser, stream, input_size.x(),
                                       input_size.y(), m_state, m_state_size,
                                       m_scratch, m_scratch_size));
    m_hdr_intensity = jit_malloc(AllocType::Device, sizeof(float));
}

MI_VARIANT Denoiser<Float, Spectrum>::~Denoiser() {
    jit_optix_check(optixDenoiserDestroy(m_denoiser));
    jit_free(m_hdr_intensity);
    jit_free(m_state);
    jit_free(m_scratch);
}

MI_VARIANT
typename Denoiser<Float, Spectrum>::TensorXf Denoiser<Float, Spectrum>::denoise(
    const TensorXf &noisy, const TensorXf *albedo, const TensorXf *normals,
    const TensorXf *previous_denoised, const TensorXf *flow) {
    using Array = typename TensorXf::Array;

    // TODO check that input matches current state

    OptixDenoiserLayer layers = {};
    OptixPixelFormat input_pixel_format = (noisy.shape(2) == 3)
                                              ? OPTIX_PIXEL_FORMAT_FLOAT3
                                              : OPTIX_PIXEL_FORMAT_FLOAT4;
    layers.input = optixImage2DfromTensor<Float, Spectrum>(
        noisy, input_pixel_format);

    layers.output = layers.input;
    Array output_data = dr::empty<Array>(noisy.size());
    layers.output.data = output_data.data();

    CUstream stream = jit_cuda_stream();

    OptixDenoiserParams params = {};
    params.hdrIntensity = m_hdr_intensity;
    params.denoiseAlpha = true;
    jit_optix_check(optixDenoiserComputeIntensity(
        m_denoiser, stream, &layers.input, m_hdr_intensity, m_scratch,
        m_scratch_size));
    params.blendFactor = 0.0f;
    params.hdrAverageColor = nullptr;

    OptixDenoiserGuideLayer guide_layer = {};

    if (m_options.guideAlbedo)
        guide_layer.albedo = optixImage2DfromTensor<Float, Spectrum>(
            *albedo, OPTIX_PIXEL_FORMAT_FLOAT3);

    std::unique_ptr<TensorXf> corrected_normals = nullptr;
    if (m_options.guideNormal) {
        // Change from right-handed coordinate system with (X=left, Y=up,
        // Z=forward) to a right-handed system  with (X=right, Y=up, Z=backward)
        corrected_normals = std::make_unique<TensorXf>(*normals);

        UInt32 x_idx = dr::arange<UInt32>(0, corrected_normals->size(), 3);
        Float x_values = dr::gather<Float>(corrected_normals->array(), x_idx);
        dr::scatter(corrected_normals->array(), -x_values, x_idx);

        UInt32 z_idx = dr::arange<UInt32>(2, corrected_normals->size(), 3);
        Float z_values = dr::gather<Float>(corrected_normals->array(), z_idx);
        dr::scatter(corrected_normals->array(), -z_values, z_idx);

        guide_layer.normal = optixImage2DfromTensor<Float, Spectrum>(
            *corrected_normals, OPTIX_PIXEL_FORMAT_FLOAT3);
    }

    if (m_temporal) {
        guide_layer.flow = optixImage2DfromTensor<Float, Spectrum>(
            *flow, OPTIX_PIXEL_FORMAT_FLOAT2);
        layers.previousOutput = optixImage2DfromTensor<Float, Spectrum>(
            *previous_denoised, input_pixel_format);
    }

    jit_optix_check(optixDenoiserInvoke(m_denoiser, stream, &params, m_state,
                                        m_state_size, &guide_layer, &layers, 1,
                                        0, 0, m_scratch, m_scratch_size));

    size_t shape[3] = { noisy.shape(0), noisy.shape(1), noisy.shape(2) };
    return TensorXf(std::move(output_data), 3, shape);
}

MI_VARIANT
ref<Bitmap> Denoiser<Float, Spectrum>::denoise(
    const ref<Bitmap> &noisy_, const std::string &albedo_ch,
    const std::string &normals_ch, const std::string &flow_ch,
    const std::string &previous_denoised_ch, const std::string &noisy_ch) {
    size_t noisy_tensor_shape[3] = { noisy_->height(), noisy_->width(),
                                     noisy_->channel_count() };
    if (noisy_->pixel_format() != Bitmap::PixelFormat::MultiChannel) {
        TensorXf noisy_tensor(noisy_->data(), 3, noisy_tensor_shape);
        TensorXf denoised =
            denoise(noisy_tensor, nullptr, nullptr, nullptr, nullptr);

        void *denoised_data =
            jit_malloc_migrate(denoised.data(), AllocType::Host, false);
        jit_sync_thread();

        return new Bitmap((denoised.shape(2) == 3) ? Bitmap::PixelFormat::RGB
                                                   : Bitmap::PixelFormat::RGBA,
                          Struct::Type::Float32,
                          { denoised.shape(1), denoised.shape(0) },
                          denoised.shape(2), {}, (uint8_t *) denoised_data);
    }

    const Bitmap *noisy_bmp = nullptr;
    const Bitmap *albedo_bmp = nullptr;
    const Bitmap *normals_bmp = nullptr;
    const Bitmap *flow_bmp = nullptr;
    const Bitmap *previous_denoised_bmp = nullptr;

    bool found_albedo = albedo_ch == "";
    bool found_normals = normals_ch == "";
    bool found_flow = flow_ch == "";
    bool found_previous_denoised = previous_denoised_ch == "";

    std::vector<std::pair<std::string, ref<Bitmap>>> res = noisy_->split();
    for (const auto &pair : res) {
        if (found_albedo && found_normals && found_flow &&
            found_previous_denoised && noisy_bmp != nullptr)
            break;
        if (!found_albedo && pair.first == albedo_ch) {
            found_albedo = true;
            albedo_bmp = pair.second.get();
        }
        if (!found_normals && pair.first == normals_ch) {
            found_normals = true;
            normals_bmp = pair.second.get();
        }
        if (!found_flow && pair.first == flow_ch) {
            found_flow = true;
            flow_bmp = pair.second.get();
        }
        if (!found_previous_denoised && pair.first == previous_denoised_ch) {
            found_previous_denoised = true;
            previous_denoised_bmp = pair.second.get();
        }
        if (noisy_bmp == nullptr && pair.first == noisy_ch)
            noisy_bmp = pair.second.get();
    }

    const auto throw_missing_channel = [&](const std::string &channel) {
        Throw("Could not find rendered image with channel name '%s' in:\n%s",
              channel, noisy_->to_string());
    };
    if (noisy_bmp == nullptr)
        throw_missing_channel(noisy_ch);
    if (!found_albedo)
        throw_missing_channel(albedo_ch);
    if (!found_normals)
        throw_missing_channel(normals_ch);
    if (!found_flow)
        throw_missing_channel(flow_ch);
    if (!found_previous_denoised)
        throw_missing_channel(previous_denoised_ch);

    noisy_tensor_shape[2] = noisy_bmp->channel_count();
    TensorXf noisy_tensor(noisy_bmp->data(), 3, noisy_tensor_shape);

    std::unique_ptr<TensorXf> albedo_tensor = nullptr;
    if (albedo_bmp != nullptr) {
        size_t shape[3] = { noisy_->height(), noisy_->width(), 3 };
        albedo_tensor =
            std::make_unique<TensorXf>(albedo_bmp->data(), 3, shape);
    }
    std::unique_ptr<TensorXf> normals_tensor = nullptr;
    if (normals_bmp != nullptr) {
        size_t shape[3] = { noisy_->height(), noisy_->width(), 3 };
        normals_tensor =
            std::make_unique<TensorXf>(normals_bmp->data(), 3, shape);
    }
    std::unique_ptr<TensorXf> flow_tensor = nullptr;
    if (flow_bmp != nullptr) {
        size_t flow_shape[3] = { noisy_->height(), noisy_->width(), 2 };
        flow_tensor =
            std::make_unique<TensorXf>(flow_bmp->data(), 3, flow_shape);
    }
    std::unique_ptr<TensorXf> previous_denoised_tensor = nullptr;
    if (previous_denoised_bmp!= nullptr)
        previous_denoised_tensor = std::make_unique<TensorXf>(
            previous_denoised_bmp->data(), 3, noisy_tensor_shape);

    TensorXf denoised =
        denoise(noisy_tensor, albedo_tensor.get(), normals_tensor.get(),
                flow_tensor.get(), previous_denoised_tensor.get());

    void *denoised_data =
        jit_malloc_migrate(denoised.data(), AllocType::Host, false);
    jit_sync_thread();

    return new Bitmap((denoised.shape(2) == 3) ? Bitmap::PixelFormat::RGB
                                               : Bitmap::PixelFormat::RGBA,
                      Struct::Type::Float32,
                      { denoised.shape(1), denoised.shape(0) },
                      denoised.shape(2), {}, (uint8_t *) denoised_data);
}

MI_VARIANT
std::string Denoiser<Float, Spectrum>::to_string() const {
    std::ostringstream oss;
    oss << "Denoiser[" << std::endl
        << "  albedo = " << m_options.guideAlbedo << "," << std::endl
        << "  normals = " << m_options.guideNormal << "," << std::endl
        << "  temporal = " << m_temporal << std::endl
        << "]";
    return oss.str();
}

MI_IMPLEMENT_CLASS_VARIANT(Denoiser, Object, "denoiser")
MI_INSTANTIATE_CLASS(Denoiser)

NAMESPACE_END(mitsuba)