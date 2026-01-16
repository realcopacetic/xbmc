/*
 *  Copyright (C) 2005-2024 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#pragma once

#include "GLESShader.h"
#include "rendering/RenderSystem.h"
#include "utils/ColorUtils.h"
#include "utils/Map.h"

#include <map>

#include <fmt/format.h>

#include "system_gl.h"

enum class ShaderMethodGLES
{
  SM_DEFAULT,
  SM_TEXTURE,
  SM_TEXTURE_111R,
  SM_MULTI,
  SM_MULTI_RGBA_111R,
  SM_FONTS,
  SM_FONTS_SHADER_CLIP,
  SM_TEXTURE_NOBLEND,
  SM_MULTI_BLENDCOLOR,
  SM_MULTI_RGBA_111R_BLENDCOLOR,
  SM_MULTI_111R_111R_BLENDCOLOR,
  SM_TEXTURE_RGBA,
  SM_TEXTURE_RGBA_OES,
  SM_TEXTURE_RGBA_BLENDCOLOR,
  SM_TEXTURE_RGBA_BOB,
  SM_TEXTURE_RGBA_BOB_OES,
  SM_TEXTURE_NOALPHA,
  SM_STENCIL_ROUNDED_MASK,
  SM_ROUNDRECT_COMPOSITE,
  SM_MAX
};

template<>
struct fmt::formatter<ShaderMethodGLES> : fmt::formatter<std::string_view>
{
  template<typename FormatContext>
  constexpr auto format(const ShaderMethodGLES& shaderMethod, FormatContext& ctx)
  {
    const auto it = ShaderMethodGLESMap.find(shaderMethod);
    if (it == ShaderMethodGLESMap.cend())
      throw std::range_error("no string mapping found for shader method");

    return fmt::formatter<string_view>::format(it->second, ctx);
  }

private:
  static constexpr auto ShaderMethodGLESMap = make_map<ShaderMethodGLES, std::string_view>({
      {ShaderMethodGLES::SM_DEFAULT, "default"},
      {ShaderMethodGLES::SM_TEXTURE, "texture"},
      {ShaderMethodGLES::SM_TEXTURE_111R, "alpha texture with diffuse color"},
      {ShaderMethodGLES::SM_MULTI, "multi"},
      {ShaderMethodGLES::SM_MULTI_RGBA_111R, "multi with color/alpha texture"},
      {ShaderMethodGLES::SM_FONTS, "fonts"},
      {ShaderMethodGLES::SM_FONTS_SHADER_CLIP, "fonts with vertex shader based clipping"},
      {ShaderMethodGLES::SM_TEXTURE_NOBLEND, "texture no blending"},
      {ShaderMethodGLES::SM_MULTI_BLENDCOLOR, "multi blend colour"},
      {ShaderMethodGLES::SM_MULTI_RGBA_111R_BLENDCOLOR,
       "multi with color/alpha texture and blend color"},
      {ShaderMethodGLES::SM_MULTI_111R_111R_BLENDCOLOR,
       "multi with alpha/alpha texture and blend color"},
      {ShaderMethodGLES::SM_TEXTURE_RGBA, "texture rgba"},
      {ShaderMethodGLES::SM_TEXTURE_RGBA_OES, "texture rgba OES"},
      {ShaderMethodGLES::SM_TEXTURE_RGBA_BLENDCOLOR, "texture rgba blend colour"},
      {ShaderMethodGLES::SM_TEXTURE_RGBA_BOB, "texture rgba bob"},
      {ShaderMethodGLES::SM_TEXTURE_RGBA_BOB_OES, "texture rgba bob OES"},
      {ShaderMethodGLES::SM_TEXTURE_NOALPHA, "texture no alpha"},
      {ShaderMethodGLES::SM_STENCIL_ROUNDED_MASK, "stencil_rounded_mask"},
      {ShaderMethodGLES::SM_ROUNDRECT_COMPOSITE, "roundrect_composite"},
  });

  static_assert(static_cast<size_t>(ShaderMethodGLES::SM_MAX) == ShaderMethodGLESMap.size(),
                "ShaderMethodGLESMap doesn't match the size of ShaderMethodGLES, did you forget to "
                "add/remove a mapping?");
};

class CRenderSystemGLES : public CRenderSystemBase
{
public:
  CRenderSystemGLES();
  ~CRenderSystemGLES() override = default;

  bool InitRenderSystem() override;
  bool DestroyRenderSystem() override;
  bool ResetRenderSystem(int width, int height) override;

  bool BeginRender() override;
  bool EndRender() override;
  void PresentRender(bool rendered, bool videoLayer) override;
  void InvalidateColorBuffer() override;
  bool ClearBuffers(KODI::UTILS::COLOR::Color color) override;
  bool IsExtSupported(const char* extension) const override;

  void SetVSync(bool vsync);
  void ResetVSync() { m_bVsyncInit = false; }

  void SetViewPort(const CRect& viewPort) override;
  void GetViewPort(CRect& viewPort) override;

  bool ScissorsCanEffectClipping() override;
  CRect ClipRectToScissorRect(const CRect &rect) override;
  void SetScissors(const CRect& rect) override;
  void ResetScissors() override;

  bool BeginStencilClip(const CRect& rectFbBL, float radiusFbPx) override;
  void EndStencilClip() override;

  bool BeginOffscreenRoundedGroup(const CRect& rectScreenTL, float radiusPx) override;
  void EndOffscreenRoundedGroup() override;
  void SetDepthCulling(DEPTH_CULLING culling) override;

  void CaptureStateBlock() override;
  void ApplyStateBlock() override;

  void SetCameraPosition(const CPoint &camera, int screenWidth, int screenHeight, float stereoFactor = 0.0f) override;

  bool SupportsStereo(RENDER_STEREO_MODE mode) const override;

  void Project(float &x, float &y, float &z) override;

  std::string GetShaderPath(const std::string& filename) override;

  void InitialiseShaders();
  void ReleaseShaders();
  void EnableGUIShader(ShaderMethodGLES method);
  void DisableGUIShader();

  GLint GUIShaderGetPos();
  GLint GUIShaderGetCol();
  GLint GUIShaderGetCoord0();
  GLint GUIShaderGetCoord1();
  GLint GUIShaderGetUniCol();
  GLint GUIShaderGetCoord0Matrix();
  GLint GUIShaderGetField();
  GLint GUIShaderGetStep();
  GLint GUIShaderGetContrast();
  GLint GUIShaderGetBrightness();
  GLint GUIShaderGetModel();
  GLint GUIShaderGetMatrix();
  GLint GUIShaderGetClip();
  GLint GUIShaderGetCoordStep();
  GLint GUIShaderGetDepth();

protected:
  virtual void SetVSyncImpl(bool enable) = 0;
  virtual void PresentRenderImpl(bool rendered) = 0;
  void CalculateMaxTexturesize();

  bool m_bVsyncInit{false};
  int m_width;
  int m_height;

  std::string m_RenderExtensions;

  std::map<ShaderMethodGLES, std::unique_ptr<CGLESShader>> m_pShader;

  // Stencil clip state (single-level by design for now)
  uint8_t m_stencilRef{0};
  GLint m_maskRectLoc{-1};
  GLint m_maskRadiusLoc{-1};
  
  // Reusing SM_STENCIL_ROUNDED_MASK program for offscreen composite as well
  GLint m_maskSamplerLoc{-1};   // m_samp0
  GLint m_maskViewportLoc{-1};  // m_viewport
  GLint m_maskAAWidthLoc{-1};   // m_aaWidth
  GLint m_maskPosLoc{-1};       // m_attrpos (already have m_roundMaskPosLoc but keep consistent)

  // Composite shader locations
  GLint m_compMaskRectLoc{-1};
  GLint m_compRadiusLoc{-1};
  GLint m_compAAWidthLoc{-1};
  GLint m_compViewportLoc{-1};
  GLint m_compSamplerLoc{-1};
  GLint m_compPosLoc{-1};

  // Offscreen rounded group helpers (GLES only)
  bool EnsureGroupFbo(int w, int h);

  // Active offscreen rounded group parameters (single-level for now)
  CRect m_offscreenRect;
  float m_offscreenRadius{0.0f};

  // Fullscreen offscreen buffer (RGBA) for rounded group rendering
  GLuint m_groupFbo{0};
  GLuint m_groupTex{0};
  int m_groupW{0};
  int m_groupH{0};
  bool m_groupActive{false}; // single-level guard
  GLint m_prevFbo{0};
  GLint m_prevViewport[4]{0, 0, 0, 0};


  // Fullscreen quad resources for stencil mask draw
  GLuint m_roundMaskVbo{0};
  GLint m_roundMaskPosLoc{-1};

  struct StencilSavedState
  {
    GLboolean stencilEnabled{GL_FALSE};
    GLint stencilFunc{GL_ALWAYS};
    GLint stencilRef{0};
    GLint stencilValueMask{0xFF};
    GLint stencilWriteMask{0xFF};
    GLint stencilFail{GL_KEEP};
    GLint stencilZFail{GL_KEEP};
    GLint stencilZPass{GL_KEEP};

    GLboolean scissorEnabled{GL_FALSE};
    GLint scissorBox[4]{0, 0, 0, 0};

    GLboolean colorMask[4]{GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE};
    GLboolean depthMask{GL_TRUE};
    GLboolean depthTestEnabled{GL_FALSE};
  };

  StencilSavedState m_stencilSaved{};
  bool m_stencilSavedValid{false};

  ShaderMethodGLES m_method = ShaderMethodGLES::SM_DEFAULT;

  GLint      m_viewPort[4];
};
