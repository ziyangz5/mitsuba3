#include <mitsuba/core/plugin.h>
#include <mitsuba/core/properties.h>
#include <mitsuba/core/transform.h>
#include <mitsuba/render/interaction.h>
#include <mitsuba/render/texture.h>

NAMESPACE_BEGIN(mitsuba)

// =======================================================================
//! @{ \name Texture implementations
// =======================================================================

MTS_VARIANT Texture<Float, Spectrum>::Texture(const Properties &props)
    : m_id(props.id()) { }

MTS_VARIANT Texture<Float, Spectrum>::~Texture() { }

MTS_VARIANT typename Texture<Float, Spectrum>::UnpolarizedSpectrum
Texture<Float, Spectrum>::eval(const SurfaceInteraction3f &, Mask) const {
    NotImplementedError("eval");
}

MTS_VARIANT std::pair<typename Texture<Float, Spectrum>::Wavelength,
                      typename Texture<Float, Spectrum>::UnpolarizedSpectrum>
Texture<Float, Spectrum>::sample_spectrum(const SurfaceInteraction3f &,
                                          const Wavelength &, Mask) const {
    NotImplementedError("sample_spectrum");
}

MTS_VARIANT typename Texture<Float, Spectrum>::Wavelength
Texture<Float, Spectrum>::pdf_spectrum(const SurfaceInteraction3f &, Mask) const {
    NotImplementedError("pdf_spectrum");
}

MTS_VARIANT Float Texture<Float, Spectrum>::eval_1(const SurfaceInteraction3f &, Mask) const {
    NotImplementedError("eval_1");
}

MTS_VARIANT typename Texture<Float, Spectrum>::Vector2f
Texture<Float, Spectrum>::eval_1_grad(const SurfaceInteraction3f &, Mask) const {
    NotImplementedError("eval_1_grad");
}

MTS_VARIANT typename Texture<Float, Spectrum>::Color3f
Texture<Float, Spectrum>::eval_3(const SurfaceInteraction3f &, Mask) const {
    NotImplementedError("eval_3");
}

MTS_VARIANT Float
Texture<Float, Spectrum>::mean() const {
    NotImplementedError("mean");
}

MTS_VARIANT
std::pair<typename Texture<Float, Spectrum>::Point2f, Float>
Texture<Float, Spectrum>::sample_position(const Point2f &sample,
                                          Mask) const {
    return { sample, 1.f };
}

MTS_VARIANT
Float Texture<Float, Spectrum>::pdf_position(const Point2f &, Mask) const {
    return 1;
}

MTS_VARIANT ref<Texture<Float, Spectrum>>
Texture<Float, Spectrum>::D65(ScalarFloat scale) {
    Properties props(is_spectral_v<Spectrum> ? "d65" : "uniform");
    props.set_float(is_spectral_v<Spectrum> ? "scale" : "value", Properties::Float(scale));
    ref<Texture> texture = PluginManager::instance()->create_object<Texture>(props);
    std::vector<ref<Object>> children = texture->expand();
    if (!children.empty())
        return (Texture *) children[0].get();
    return texture;
}

MTS_VARIANT typename Texture<Float, Spectrum>::ScalarVector2i
Texture<Float, Spectrum>::resolution() const {
    return ScalarVector2i(1, 1);
}

MTS_VARIANT typename Texture<Float, Spectrum>::ScalarFloat
Texture<Float, Spectrum>::spectral_resolution() const {
    NotImplementedError("spectral_resolution");
}

MTS_VARIANT typename Texture<Float, Spectrum>::ScalarVector2f
Texture<Float, Spectrum>::wavelength_range() const {
    return ScalarVector2f(MTS_CIE_MIN, MTS_CIE_MAX);
}

//! @}
// =======================================================================

MTS_IMPLEMENT_CLASS_VARIANT(Texture, Object, "texture")

MTS_INSTANTIATE_CLASS(Texture)
NAMESPACE_END(mitsuba)