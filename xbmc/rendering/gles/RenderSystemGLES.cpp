/*
 *  Copyright (C) 2005-2024 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include "RenderSystemGLES.h"

#include "URL.h"
#include "guilib/DirtyRegion.h"
#include "guilib/GUITextureGLES.h"
#include "platform/MessagePrinter.h"
#include "rendering/GLExtensions.h"
#include "rendering/MatrixGL.h"
#include "settings/AdvancedSettings.h"
#include "settings/SettingsComponent.h"
#include "utils/FileUtils.h"
#include "utils/GLUtils.h"
#include "utils/MathUtils.h"
#include "utils/SystemInfo.h"
#include "utils/TimeUtils.h"
#include "utils/XTimeUtils.h"
#include "utils/log.h"
#include "windowing/GraphicContext.h"

#include <algorithm>
#include <cmath>
#include <array>

#if defined(TARGET_LINUX)
#include "utils/EGLUtils.h"
#endif

using namespace std::chrono_literals;

CRenderSystemGLES::CRenderSystemGLES()
 : CRenderSystemBase()
{
}

static void LogGlError(const char* tag)
{
  const GLenum err = glGetError();
  if (err != GL_NO_ERROR)
    CLog::Log(LOGERROR, "GLES: GL error after {}: 0x{:04x}", tag, static_cast<unsigned int>(err));
}

bool CRenderSystemGLES::InitRenderSystem()
{
  GLint maxTextureSize;

  glGetIntegerv(GL_MAX_TEXTURE_SIZE, &maxTextureSize);

  m_maxTextureSize = maxTextureSize;

  // Get the GLES version number
  m_RenderVersion = "<none>";
  m_RenderVersionMajor = 0;
  m_RenderVersionMinor = 0;

  const char* ver = (const char*)glGetString(GL_VERSION);
  if (ver != NULL)
  {
    sscanf(ver, "%d.%d", &m_RenderVersionMajor, &m_RenderVersionMinor);
    if (!m_RenderVersionMajor)
      sscanf(ver, "%*s %*s %d.%d", &m_RenderVersionMajor, &m_RenderVersionMinor);
    m_RenderVersion = ver;
  }

  // Get our driver vendor and renderer
  const char *tmpVendor = (const char*) glGetString(GL_VENDOR);
  m_RenderVendor.clear();
  if (tmpVendor != NULL)
    m_RenderVendor = tmpVendor;

  const char *tmpRenderer = (const char*) glGetString(GL_RENDERER);
  m_RenderRenderer.clear();
  if (tmpRenderer != NULL)
    m_RenderRenderer = tmpRenderer;

  m_RenderExtensions = "";

  const char *tmpExtensions = (const char*) glGetString(GL_EXTENSIONS);
  if (tmpExtensions != NULL)
  {
    m_RenderExtensions += tmpExtensions;
    m_RenderExtensions += " ";
  }

#if defined(GL_KHR_debug) && defined(TARGET_LINUX)
  if (CServiceBroker::GetSettingsComponent()->GetAdvancedSettings()->m_openGlDebugging)
  {
    if (CGLExtensions::IsExtensionSupported(CGLExtensions::KHR_debug))
    {
      auto glDebugMessageCallback = CEGLUtils::GetRequiredProcAddress<PFNGLDEBUGMESSAGECALLBACKKHRPROC>("glDebugMessageCallbackKHR");
      auto glDebugMessageControl = CEGLUtils::GetRequiredProcAddress<PFNGLDEBUGMESSAGECONTROLKHRPROC>("glDebugMessageControlKHR");

      glEnable(GL_DEBUG_OUTPUT_SYNCHRONOUS_KHR);
      glDebugMessageCallback(KODI::UTILS::GL::GlErrorCallback, nullptr);

      // ignore shader compilation information
      glDebugMessageControl(GL_DEBUG_SOURCE_SHADER_COMPILER_KHR, GL_DEBUG_TYPE_OTHER_KHR, GL_DONT_CARE, 0, nullptr, GL_FALSE);

      CLog::Log(LOGDEBUG, "OpenGL(ES): debugging enabled");
    }
    else
    {
      CLog::Log(LOGDEBUG, "OpenGL(ES): debugging requested but the required extension isn't available (GL_KHR_debug)");
    }
  }
#endif

  // Shut down gracefully if OpenGL context could not be allocated
  if (m_RenderVersionMajor == 0)
  {
    CLog::Log(LOGFATAL, "Can not initialize OpenGL context. Exiting");
    CMessagePrinter::DisplayError("ERROR: Can not initialize OpenGL context. Exiting");
    return false;
  }

  LogGraphicsInfo();

  m_bRenderCreated = true;

  InitialiseShaders();

  CGUITextureGLES::Register();

  return true;
}

bool CRenderSystemGLES::ResetRenderSystem(int width, int height)
{
  m_width = width;
  m_height = height;

  glClearColor( 0.0f, 0.0f, 0.0f, 0.0f );
  CalculateMaxTexturesize();

  CRect rect( 0, 0, width, height );
  SetViewPort( rect );

  glEnable(GL_SCISSOR_TEST);

  glMatrixProject.Clear();
  glMatrixProject->LoadIdentity();
  glMatrixProject->Ortho(0.0f, width-1, height-1, 0.0f, -1.0f, 1.0f);
  glMatrixProject.Load();

  glMatrixModview.Clear();
  glMatrixModview->LoadIdentity();
  glMatrixModview.Load();

  glMatrixTexture.Clear();
  glMatrixTexture->LoadIdentity();
  glMatrixTexture.Load();

  glBlendFunc(GL_SRC_ALPHA, GL_ONE);
  glEnable(GL_BLEND); // Turn Blending On

  return true;
}

bool CRenderSystemGLES::DestroyRenderSystem()
{
  ResetScissors();
  CDirtyRegionList dirtyRegions;
  CDirtyRegion dirtyWindow(CServiceBroker::GetWinSystem()->GetGfxContext().GetViewWindow());
  dirtyRegions.push_back(dirtyWindow);

  ClearBuffers(0);
  glFinish();
  PresentRenderImpl(true);

  ReleaseShaders();
  m_bRenderCreated = false;

  return true;
}

bool CRenderSystemGLES::BeginRender()
{
  if (!m_bRenderCreated)
    return false;

  const bool useLimited = CServiceBroker::GetWinSystem()->UseLimitedColor();
  const bool usePQ = CServiceBroker::GetWinSystem()->GetGfxContext().IsTransferPQ();

  if (m_limitedColorRange != useLimited || m_transferPQ != usePQ)
  {
    ReleaseShaders();

    m_limitedColorRange = useLimited;
    m_transferPQ = usePQ;

    InitialiseShaders();
  }

  return true;
}

bool CRenderSystemGLES::EndRender()
{
  if (!m_bRenderCreated)
    return false;

  return true;
}

void CRenderSystemGLES::InvalidateColorBuffer()
{
  if (!m_bRenderCreated)
    return;

  // some platforms prefer a clear, instead of rendering over
  if (!CServiceBroker::GetSettingsComponent()->GetAdvancedSettings()->m_guiGeometryClear)
    ClearBuffers(0);

  if (!CServiceBroker::GetSettingsComponent()->GetAdvancedSettings()->m_guiFrontToBackRendering)
    return;

  glClearDepthf(0);
  glDepthMask(true);
  glClear(GL_DEPTH_BUFFER_BIT);
}

bool CRenderSystemGLES::ClearBuffers(KODI::UTILS::COLOR::Color color)
{
  if (!m_bRenderCreated)
    return false;

  float r = KODI::UTILS::GL::GetChannelFromARGB(KODI::UTILS::GL::ColorChannel::R, color) / 255.0f;
  float g = KODI::UTILS::GL::GetChannelFromARGB(KODI::UTILS::GL::ColorChannel::G, color) / 255.0f;
  float b = KODI::UTILS::GL::GetChannelFromARGB(KODI::UTILS::GL::ColorChannel::B, color) / 255.0f;
  float a = KODI::UTILS::GL::GetChannelFromARGB(KODI::UTILS::GL::ColorChannel::A, color) / 255.0f;

  glClearColor(r, g, b, a);

  GLbitfield flags = GL_COLOR_BUFFER_BIT;

  if (CServiceBroker::GetSettingsComponent()->GetAdvancedSettings()->m_guiFrontToBackRendering)
  {
    glClearDepthf(0);
    glDepthMask(GL_TRUE);
    flags |= GL_DEPTH_BUFFER_BIT;
  }

  glClear(flags);

  return true;
}

bool CRenderSystemGLES::IsExtSupported(const char* extension) const
{
  if (strcmp( extension, "GL_EXT_framebuffer_object" ) == 0)
  {
    // GLES has FBO as a core element, not an extension!
    return true;
  }
  else
  {
    std::string name;
    name  = " ";
    name += extension;
    name += " ";

    return m_RenderExtensions.find(name) != std::string::npos;
  }
}

void CRenderSystemGLES::PresentRender(bool rendered, bool videoLayer)
{
  SetVSync(true);

  if (!m_bRenderCreated)
    return;

  PresentRenderImpl(rendered);

  // if video is rendered to a separate layer, we should not block this thread
  if (!rendered && !videoLayer)
    KODI::TIME::Sleep(40ms);
}

void CRenderSystemGLES::SetVSync(bool enable)
{
  if (m_bVsyncInit)
    return;

  if (!m_bRenderCreated)
    return;

  if (enable)
    CLog::Log(LOGINFO, "GLES: Enabling VSYNC");
  else
    CLog::Log(LOGINFO, "GLES: Disabling VSYNC");

  m_bVsyncInit = true;

  SetVSyncImpl(enable);
}

void CRenderSystemGLES::CaptureStateBlock()
{
  if (!m_bRenderCreated)
    return;

  glMatrixProject.Push();
  glMatrixModview.Push();
  glMatrixTexture.Push();

  glDisable(GL_SCISSOR_TEST); // fixes FBO corruption on Macs
  glActiveTexture(GL_TEXTURE0);
//! @todo - NOTE: Only for Screensavers & Visualisations
//  glColor3f(1.0, 1.0, 1.0);
}

void CRenderSystemGLES::ApplyStateBlock()
{
  if (!m_bRenderCreated)
    return;

  glMatrixProject.PopLoad();
  glMatrixModview.PopLoad();
  glMatrixTexture.PopLoad();
  glActiveTexture(GL_TEXTURE0);
  glEnable(GL_BLEND);
  glEnable(GL_SCISSOR_TEST);
  glClear(GL_DEPTH_BUFFER_BIT);
}

void CRenderSystemGLES::SetCameraPosition(const CPoint &camera, int screenWidth, int screenHeight, float stereoFactor)
{
  if (!m_bRenderCreated)
    return;

  CPoint offset = camera - CPoint(screenWidth*0.5f, screenHeight*0.5f);

  float w = (float)m_viewPort[2]*0.5f;
  float h = (float)m_viewPort[3]*0.5f;

  glMatrixModview->LoadIdentity();
  glMatrixModview->Translatef(-(w + offset.x - stereoFactor), +(h + offset.y), 0);
  glMatrixModview->LookAt(0.0f, 0.0f, -2.0f * h, 0.0f, 0.0f, 0.0f, 0.0f, -1.0f, 0.0f);
  glMatrixModview.Load();

  glMatrixProject->LoadIdentity();
  glMatrixProject->Frustum( (-w - offset.x)*0.5f, (w - offset.x)*0.5f, (-h + offset.y)*0.5f, (h + offset.y)*0.5f, h, 100*h);
  glMatrixProject.Load();
}

void CRenderSystemGLES::Project(float &x, float &y, float &z)
{
  GLfloat coordX, coordY, coordZ;
  if (CMatrixGL::Project(x, y, z, glMatrixModview.Get(), glMatrixProject.Get(), m_viewPort, &coordX, &coordY, &coordZ))
  {
    x = coordX;
    y = (float)(m_viewPort[1] + m_viewPort[3] - coordY);
    z = 0;
  }
}

void CRenderSystemGLES::CalculateMaxTexturesize()
{
  // GLES cannot do PROXY textures to determine maximum size,
  CLog::Log(LOGINFO, "GLES: Maximum texture width: {}", m_maxTextureSize);
}

void CRenderSystemGLES::GetViewPort(CRect& viewPort)
{
  if (!m_bRenderCreated)
    return;

  viewPort.x1 = m_viewPort[0];
  viewPort.y1 = m_height - m_viewPort[1] - m_viewPort[3];
  viewPort.x2 = m_viewPort[0] + m_viewPort[2];
  viewPort.y2 = viewPort.y1 + m_viewPort[3];
}

void CRenderSystemGLES::SetViewPort(const CRect& viewPort)
{
  if (!m_bRenderCreated)
    return;

  glScissor((GLint) viewPort.x1, (GLint) (m_height - viewPort.y1 - viewPort.Height()), (GLsizei) viewPort.Width(), (GLsizei) viewPort.Height());
  glViewport((GLint) viewPort.x1, (GLint) (m_height - viewPort.y1 - viewPort.Height()), (GLsizei) viewPort.Width(), (GLsizei) viewPort.Height());
  m_viewPort[0] = viewPort.x1;
  m_viewPort[1] = m_height - viewPort.y1 - viewPort.Height();
  m_viewPort[2] = viewPort.Width();
  m_viewPort[3] = viewPort.Height();
}

bool CRenderSystemGLES::ScissorsCanEffectClipping()
{
  if (m_pShader[m_method])
    return m_pShader[m_method]->HardwareClipIsPossible();

  return false;
}

CRect CRenderSystemGLES::ClipRectToScissorRect(const CRect &rect)
{
  if (!m_pShader[m_method])
    return CRect();
  float xFactor = m_pShader[m_method]->GetClipXFactor();
  float xOffset = m_pShader[m_method]->GetClipXOffset();
  float yFactor = m_pShader[m_method]->GetClipYFactor();
  float yOffset = m_pShader[m_method]->GetClipYOffset();
  return CRect(rect.x1 * xFactor + xOffset,
               rect.y1 * yFactor + yOffset,
               rect.x2 * xFactor + xOffset,
               rect.y2 * yFactor + yOffset);
}

void CRenderSystemGLES::SetScissors(const CRect &rect)
{
  if (!m_bRenderCreated)
    return;
  GLint x1 = MathUtils::round_int(static_cast<double>(rect.x1));
  GLint y1 = MathUtils::round_int(static_cast<double>(rect.y1));
  GLint x2 = MathUtils::round_int(static_cast<double>(rect.x2));
  GLint y2 = MathUtils::round_int(static_cast<double>(rect.y2));
  glScissor(x1, m_height - y2, x2-x1, y2-y1);
}

void CRenderSystemGLES::ResetScissors()
{
  SetScissors(CRect(0, 0, (float)m_width, (float)m_height));
}

void CRenderSystemGLES::SetDepthCulling(DEPTH_CULLING culling)
{
  if (culling == DEPTH_CULLING_OFF)
  {
    glDisable(GL_DEPTH_TEST);
    glDepthMask(GL_FALSE);
  }
  else if (culling == DEPTH_CULLING_BACK_TO_FRONT)
  {
    glEnable(GL_DEPTH_TEST);
    glDepthMask(GL_FALSE);
    glDepthFunc(GL_GEQUAL);
  }
  else if (culling == DEPTH_CULLING_FRONT_TO_BACK)
  {
    glEnable(GL_DEPTH_TEST);
    glDepthMask(GL_TRUE);
    glDepthFunc(GL_GREATER);
  }
}

static bool HasStencilBufferForCurrentDrawTarget()
{
  GLint fbo = 0;
  glGetIntegerv(GL_FRAMEBUFFER_BINDING, &fbo);

  if (fbo == 0)
  {
    GLint stencilBits = 0;
    glGetIntegerv(GL_STENCIL_BITS, &stencilBits);
    return stencilBits > 0;
  }

  GLint objType = GL_NONE;

  glGetFramebufferAttachmentParameteriv(GL_FRAMEBUFFER,
                                        GL_DEPTH_STENCIL_ATTACHMENT,
                                        GL_FRAMEBUFFER_ATTACHMENT_OBJECT_TYPE,
                                        &objType);
  if (objType != GL_NONE)
    return true;

  glGetFramebufferAttachmentParameteriv(GL_FRAMEBUFFER,
                                        GL_STENCIL_ATTACHMENT,
                                        GL_FRAMEBUFFER_ATTACHMENT_OBJECT_TYPE,
                                        &objType);
  return objType != GL_NONE;
}

void CRenderSystemGLES::InitialiseShaders()
{
  std::string defines;
  m_limitedColorRange = CServiceBroker::GetWinSystem()->UseLimitedColor();
  if (m_limitedColorRange)
  {
    defines += "#define KODI_LIMITED_RANGE 1\n";
  }

  if (m_transferPQ)
  {
    defines += "#define KODI_TRANSFER_PQ 1\n";
  }

  m_pShader[ShaderMethodGLES::SM_DEFAULT] =
      std::make_unique<CGLESShader>("gles_shader.vert", "gles_shader_default.frag", defines);
  if (!m_pShader[ShaderMethodGLES::SM_DEFAULT]->CompileAndLink())
  {
    m_pShader[ShaderMethodGLES::SM_DEFAULT]->Free();
    m_pShader[ShaderMethodGLES::SM_DEFAULT].reset();
    CLog::Log(LOGERROR, "GUI Shader gles_shader_default.frag - compile and link failed");
  }

  m_pShader[ShaderMethodGLES::SM_TEXTURE] =
      std::make_unique<CGLESShader>("gles_shader_texture.frag", defines);
  if (!m_pShader[ShaderMethodGLES::SM_TEXTURE]->CompileAndLink())
  {
    m_pShader[ShaderMethodGLES::SM_TEXTURE]->Free();
    m_pShader[ShaderMethodGLES::SM_TEXTURE].reset();
    CLog::Log(LOGERROR, "GUI Shader gles_shader_texture.frag - compile and link failed");
  }

  m_pShader[ShaderMethodGLES::SM_TEXTURE_111R] =
      std::make_unique<CGLESShader>("gles_shader_texture_111r.frag", defines);
  if (!m_pShader[ShaderMethodGLES::SM_TEXTURE_111R]->CompileAndLink())
  {
    m_pShader[ShaderMethodGLES::SM_TEXTURE_111R]->Free();
    m_pShader[ShaderMethodGLES::SM_TEXTURE_111R].reset();
    CLog::Log(LOGERROR, "GUI Shader gles_shader_texture_111r.frag - compile and link failed");
  }

  m_pShader[ShaderMethodGLES::SM_MULTI] =
      std::make_unique<CGLESShader>("gles_shader_multi.frag", defines);
  if (!m_pShader[ShaderMethodGLES::SM_MULTI]->CompileAndLink())
  {
    m_pShader[ShaderMethodGLES::SM_MULTI]->Free();
    m_pShader[ShaderMethodGLES::SM_MULTI].reset();
    CLog::Log(LOGERROR, "GUI Shader gles_shader_multi.frag - compile and link failed");
  }

  m_pShader[ShaderMethodGLES::SM_MULTI_RGBA_111R] =
      std::make_unique<CGLESShader>("gles_shader_multi_rgba_111r.frag", defines);
  if (!m_pShader[ShaderMethodGLES::SM_MULTI_RGBA_111R]->CompileAndLink())
  {
    m_pShader[ShaderMethodGLES::SM_MULTI_RGBA_111R]->Free();
    m_pShader[ShaderMethodGLES::SM_MULTI_RGBA_111R].reset();
    CLog::Log(LOGERROR, "GUI Shader gles_shader_multi_rgba_111r.frag - compile and link failed");
  }

  m_pShader[ShaderMethodGLES::SM_FONTS] =
      std::make_unique<CGLESShader>("gles_shader_simple.vert", "gles_shader_fonts.frag", defines);
  if (!m_pShader[ShaderMethodGLES::SM_FONTS]->CompileAndLink())
  {
    m_pShader[ShaderMethodGLES::SM_FONTS]->Free();
    m_pShader[ShaderMethodGLES::SM_FONTS].reset();
    CLog::Log(LOGERROR, "GUI Shader gles_shader_fonts.frag - compile and link failed");
  }

  m_pShader[ShaderMethodGLES::SM_FONTS_SHADER_CLIP] =
      std::make_unique<CGLESShader>("gles_shader_clip.vert", "gles_shader_fonts.frag", defines);
  if (!m_pShader[ShaderMethodGLES::SM_FONTS_SHADER_CLIP]->CompileAndLink())
  {
    m_pShader[ShaderMethodGLES::SM_FONTS_SHADER_CLIP]->Free();
    m_pShader[ShaderMethodGLES::SM_FONTS_SHADER_CLIP].reset();
    CLog::Log(LOGERROR, "GUI Shader gles_shader_clip.vert + gles_shader_fonts.frag - compile "
                        "and link failed");
  }

  m_pShader[ShaderMethodGLES::SM_TEXTURE_NOBLEND] =
      std::make_unique<CGLESShader>("gles_shader_texture_noblend.frag", defines);
  if (!m_pShader[ShaderMethodGLES::SM_TEXTURE_NOBLEND]->CompileAndLink())
  {
    m_pShader[ShaderMethodGLES::SM_TEXTURE_NOBLEND]->Free();
    m_pShader[ShaderMethodGLES::SM_TEXTURE_NOBLEND].reset();
    CLog::Log(LOGERROR, "GUI Shader gles_shader_texture_noblend.frag - compile and link failed");
  }

  m_pShader[ShaderMethodGLES::SM_MULTI_BLENDCOLOR] =
      std::make_unique<CGLESShader>("gles_shader_multi_blendcolor.frag", defines);
  if (!m_pShader[ShaderMethodGLES::SM_MULTI_BLENDCOLOR]->CompileAndLink())
  {
    m_pShader[ShaderMethodGLES::SM_MULTI_BLENDCOLOR]->Free();
    m_pShader[ShaderMethodGLES::SM_MULTI_BLENDCOLOR].reset();
    CLog::Log(LOGERROR, "GUI Shader gles_shader_multi_blendcolor.frag - compile and link failed");
  }

  m_pShader[ShaderMethodGLES::SM_MULTI_RGBA_111R_BLENDCOLOR] =
      std::make_unique<CGLESShader>("gles_shader_multi_rgba_111r_blendcolor.frag", defines);
  if (!m_pShader[ShaderMethodGLES::SM_MULTI_RGBA_111R_BLENDCOLOR]->CompileAndLink())
  {
    m_pShader[ShaderMethodGLES::SM_MULTI_RGBA_111R_BLENDCOLOR]->Free();
    m_pShader[ShaderMethodGLES::SM_MULTI_RGBA_111R_BLENDCOLOR].reset();
    CLog::Log(LOGERROR,
              "GUI Shader gles_shader_multi_rgba_111r_blendcolor.frag - compile and link failed");
  }

  m_pShader[ShaderMethodGLES::SM_MULTI_111R_111R_BLENDCOLOR] =
      std::make_unique<CGLESShader>("gles_shader_multi_111r_111r_blendcolor.frag", defines);
  if (!m_pShader[ShaderMethodGLES::SM_MULTI_111R_111R_BLENDCOLOR]->CompileAndLink())
  {
    m_pShader[ShaderMethodGLES::SM_MULTI_111R_111R_BLENDCOLOR]->Free();
    m_pShader[ShaderMethodGLES::SM_MULTI_111R_111R_BLENDCOLOR].reset();
    CLog::Log(LOGERROR,
              "GUI Shader gles_shader_multi_111r_111r_blendcolor.frag - compile and link failed");
  }

  m_pShader[ShaderMethodGLES::SM_TEXTURE_RGBA] =
      std::make_unique<CGLESShader>("gles_shader_rgba.frag", defines);
  if (!m_pShader[ShaderMethodGLES::SM_TEXTURE_RGBA]->CompileAndLink())
  {
    m_pShader[ShaderMethodGLES::SM_TEXTURE_RGBA]->Free();
    m_pShader[ShaderMethodGLES::SM_TEXTURE_RGBA].reset();
    CLog::Log(LOGERROR, "GUI Shader gles_shader_rgba.frag - compile and link failed");
  }

  m_pShader[ShaderMethodGLES::SM_TEXTURE_RGBA_BLENDCOLOR] =
      std::make_unique<CGLESShader>("gles_shader_rgba_blendcolor.frag", defines);
  if (!m_pShader[ShaderMethodGLES::SM_TEXTURE_RGBA_BLENDCOLOR]->CompileAndLink())
  {
    m_pShader[ShaderMethodGLES::SM_TEXTURE_RGBA_BLENDCOLOR]->Free();
    m_pShader[ShaderMethodGLES::SM_TEXTURE_RGBA_BLENDCOLOR].reset();
    CLog::Log(LOGERROR, "GUI Shader gles_shader_rgba_blendcolor.frag - compile and link failed");
  }

  m_pShader[ShaderMethodGLES::SM_TEXTURE_RGBA_BOB] =
      std::make_unique<CGLESShader>("gles_shader_rgba_bob.frag", defines);
  if (!m_pShader[ShaderMethodGLES::SM_TEXTURE_RGBA_BOB]->CompileAndLink())
  {
    m_pShader[ShaderMethodGLES::SM_TEXTURE_RGBA_BOB]->Free();
    m_pShader[ShaderMethodGLES::SM_TEXTURE_RGBA_BOB].reset();
    CLog::Log(LOGERROR, "GUI Shader gles_shader_rgba_bob.frag - compile and link failed");
  }

  if (CGLExtensions::IsExtensionSupported(CGLExtensions::OES_EGL_image_external))
  {
    m_pShader[ShaderMethodGLES::SM_TEXTURE_RGBA_OES] =
        std::make_unique<CGLESShader>("gles_shader_rgba_oes.frag", defines);
    if (!m_pShader[ShaderMethodGLES::SM_TEXTURE_RGBA_OES]->CompileAndLink())
    {
      m_pShader[ShaderMethodGLES::SM_TEXTURE_RGBA_OES]->Free();
      m_pShader[ShaderMethodGLES::SM_TEXTURE_RGBA_OES].reset();
      CLog::Log(LOGERROR, "GUI Shader gles_shader_rgba_oes.frag - compile and link failed");
    }


    m_pShader[ShaderMethodGLES::SM_TEXTURE_RGBA_BOB_OES] =
        std::make_unique<CGLESShader>("gles_shader_rgba_bob_oes.frag", defines);
    if (!m_pShader[ShaderMethodGLES::SM_TEXTURE_RGBA_BOB_OES]->CompileAndLink())
    {
      m_pShader[ShaderMethodGLES::SM_TEXTURE_RGBA_BOB_OES]->Free();
      m_pShader[ShaderMethodGLES::SM_TEXTURE_RGBA_BOB_OES].reset();
      CLog::Log(LOGERROR, "GUI Shader gles_shader_rgba_bob_oes.frag - compile and link failed");
    }
  }

  m_pShader[ShaderMethodGLES::SM_TEXTURE_NOALPHA] =
      std::make_unique<CGLESShader>("gles_shader_texture_noalpha.frag", defines);
  if (!m_pShader[ShaderMethodGLES::SM_TEXTURE_NOALPHA]->CompileAndLink())
  {
    m_pShader[ShaderMethodGLES::SM_TEXTURE_NOALPHA]->Free();
    m_pShader[ShaderMethodGLES::SM_TEXTURE_NOALPHA].reset();
    CLog::Log(LOGERROR, "GUI Shader gles_shader_texture_noalpha.frag - compile and link failed");
  }

  m_pShader[ShaderMethodGLES::SM_STENCIL_ROUNDED_MASK] = std::make_unique<CGLESShader>(
      "gles_shader_roundrect_mask.vert", "gles_shader_roundrect_mask.frag", defines);
  if (!m_pShader[ShaderMethodGLES::SM_STENCIL_ROUNDED_MASK]->CompileAndLink())
  {
    m_pShader[ShaderMethodGLES::SM_STENCIL_ROUNDED_MASK]->Free();
    m_pShader[ShaderMethodGLES::SM_STENCIL_ROUNDED_MASK].reset();
    CLog::Log(LOGERROR, "GUI Shader gles_shader_roundrect_mask.vert "
                        "gles_shader_roundrect_mask.frag - compile and link failed");
  }
  else
  {
    CLog::Log(LOGINFO, "GLES GUI Shader roundrect mask compiled OK");
    const GLuint prog = m_pShader[ShaderMethodGLES::SM_STENCIL_ROUNDED_MASK]->ProgramHandle();
    m_maskRectLoc = glGetUniformLocation(prog, "m_maskRect");
    m_maskRadiusLoc = glGetUniformLocation(prog, "m_radius");
    // Extra uniforms used by offscreen composite (shader reuse)
    m_maskSamplerLoc = glGetUniformLocation(prog, "m_samp0");
    m_maskViewportLoc = glGetUniformLocation(prog, "m_viewport");
    m_maskAAWidthLoc = glGetUniformLocation(prog, "m_aaWidth");
    m_maskPosLoc = glGetAttribLocation(prog, "m_attrpos");
    m_roundMaskPosLoc = glGetAttribLocation(prog, "m_attrpos");

    CLog::Log(LOGINFO,
              "GLES: roundrect(maskcomposite) prog={} pos={} samp={} vp={} rect={} rad={} aa={}",
              prog, m_maskPosLoc, m_maskSamplerLoc, m_maskViewportLoc,
              m_maskRectLoc, m_maskRadiusLoc, m_maskAAWidthLoc);

    m_pShader[ShaderMethodGLES::SM_ROUNDRECT_COMPOSITE] = std::make_unique<CGLESShader>(
        "gles_shader_roundrect_composite.vert", "gles_shader_roundrect_composite.frag", defines);
    if (!m_pShader[ShaderMethodGLES::SM_ROUNDRECT_COMPOSITE]->CompileAndLink())
    {
      m_pShader[ShaderMethodGLES::SM_ROUNDRECT_COMPOSITE]->Free();
      m_pShader[ShaderMethodGLES::SM_ROUNDRECT_COMPOSITE].reset();
      CLog::Log(LOGERROR, "GLES GUI Shader gles_shader_roundrect_composite.vert "
                          "gles_shader_roundrect_composite.frag - compile and link failed");
    }
    else
    {
      const GLuint p = m_pShader[ShaderMethodGLES::SM_ROUNDRECT_COMPOSITE]->ProgramHandle();
      m_compMaskRectLoc = glGetUniformLocation(p, "m_maskRect");
      m_compRadiusLoc = glGetUniformLocation(p, "m_radius");
      m_compAAWidthLoc = glGetUniformLocation(p, "m_aaWidth");
      m_compViewportLoc = glGetUniformLocation(p, "m_viewport");
      m_compSamplerLoc = glGetUniformLocation(p, "m_samp0");
      m_compPosLoc = glGetAttribLocation(p, "m_attrpos");
 
      CLog::Log(LOGINFO,
               "GLES: roundrect composite shader OK prog={} posLoc={} sampLoc={} vpLoc={} rectLoc={} radLoc={} aaLoc={}",
               p,
               m_compPosLoc,
               m_compSamplerLoc,
               m_compViewportLoc,
               m_compMaskRectLoc,
               m_compRadiusLoc,
               m_compAAWidthLoc);
    }
  }
}

void CRenderSystemGLES::ReleaseShaders()
{
  if (m_pShader[ShaderMethodGLES::SM_DEFAULT])
    m_pShader[ShaderMethodGLES::SM_DEFAULT]->Free();
  m_pShader[ShaderMethodGLES::SM_DEFAULT].reset();

  if (m_pShader[ShaderMethodGLES::SM_TEXTURE])
    m_pShader[ShaderMethodGLES::SM_TEXTURE]->Free();
  m_pShader[ShaderMethodGLES::SM_TEXTURE].reset();

  if (m_pShader[ShaderMethodGLES::SM_TEXTURE_111R])
    m_pShader[ShaderMethodGLES::SM_TEXTURE_111R]->Free();
  m_pShader[ShaderMethodGLES::SM_TEXTURE_111R].reset();

  if (m_pShader[ShaderMethodGLES::SM_MULTI])
    m_pShader[ShaderMethodGLES::SM_MULTI]->Free();
  m_pShader[ShaderMethodGLES::SM_MULTI].reset();

  if (m_pShader[ShaderMethodGLES::SM_MULTI_RGBA_111R])
    m_pShader[ShaderMethodGLES::SM_MULTI_RGBA_111R]->Free();
  m_pShader[ShaderMethodGLES::SM_MULTI_RGBA_111R].reset();

  if (m_pShader[ShaderMethodGLES::SM_FONTS])
    m_pShader[ShaderMethodGLES::SM_FONTS]->Free();
  m_pShader[ShaderMethodGLES::SM_FONTS].reset();

  if (m_pShader[ShaderMethodGLES::SM_FONTS_SHADER_CLIP])
    m_pShader[ShaderMethodGLES::SM_FONTS_SHADER_CLIP]->Free();
  m_pShader[ShaderMethodGLES::SM_FONTS_SHADER_CLIP].reset();

  if (m_pShader[ShaderMethodGLES::SM_TEXTURE_NOBLEND])
    m_pShader[ShaderMethodGLES::SM_TEXTURE_NOBLEND]->Free();
  m_pShader[ShaderMethodGLES::SM_TEXTURE_NOBLEND].reset();

  if (m_pShader[ShaderMethodGLES::SM_MULTI_BLENDCOLOR])
    m_pShader[ShaderMethodGLES::SM_MULTI_BLENDCOLOR]->Free();
  m_pShader[ShaderMethodGLES::SM_MULTI_BLENDCOLOR].reset();

  if (m_pShader[ShaderMethodGLES::SM_MULTI_RGBA_111R_BLENDCOLOR])
    m_pShader[ShaderMethodGLES::SM_MULTI_RGBA_111R_BLENDCOLOR]->Free();
  m_pShader[ShaderMethodGLES::SM_MULTI_RGBA_111R_BLENDCOLOR].reset();

  if (m_pShader[ShaderMethodGLES::SM_MULTI_111R_111R_BLENDCOLOR])
    m_pShader[ShaderMethodGLES::SM_MULTI_111R_111R_BLENDCOLOR]->Free();
  m_pShader[ShaderMethodGLES::SM_MULTI_111R_111R_BLENDCOLOR].reset();

  if (m_pShader[ShaderMethodGLES::SM_TEXTURE_RGBA])
    m_pShader[ShaderMethodGLES::SM_TEXTURE_RGBA]->Free();
  m_pShader[ShaderMethodGLES::SM_TEXTURE_RGBA].reset();

  if (m_pShader[ShaderMethodGLES::SM_TEXTURE_RGBA_BLENDCOLOR])
    m_pShader[ShaderMethodGLES::SM_TEXTURE_RGBA_BLENDCOLOR]->Free();
  m_pShader[ShaderMethodGLES::SM_TEXTURE_RGBA_BLENDCOLOR].reset();

  if (m_pShader[ShaderMethodGLES::SM_TEXTURE_RGBA_BOB])
    m_pShader[ShaderMethodGLES::SM_TEXTURE_RGBA_BOB]->Free();
  m_pShader[ShaderMethodGLES::SM_TEXTURE_RGBA_BOB].reset();

  if (m_pShader[ShaderMethodGLES::SM_TEXTURE_RGBA_OES])
    m_pShader[ShaderMethodGLES::SM_TEXTURE_RGBA_OES]->Free();
  m_pShader[ShaderMethodGLES::SM_TEXTURE_RGBA_OES].reset();

  if (m_pShader[ShaderMethodGLES::SM_TEXTURE_RGBA_BOB_OES])
    m_pShader[ShaderMethodGLES::SM_TEXTURE_RGBA_BOB_OES]->Free();
  m_pShader[ShaderMethodGLES::SM_TEXTURE_RGBA_BOB_OES].reset();

  if (m_pShader[ShaderMethodGLES::SM_TEXTURE_NOALPHA])
    m_pShader[ShaderMethodGLES::SM_TEXTURE_NOALPHA]->Free();
  m_pShader[ShaderMethodGLES::SM_TEXTURE_NOALPHA].reset();

  // Release rounded composite shader
  if (m_pShader[ShaderMethodGLES::SM_ROUNDRECT_COMPOSITE])
    m_pShader[ShaderMethodGLES::SM_ROUNDRECT_COMPOSITE]->Free();
  m_pShader[ShaderMethodGLES::SM_ROUNDRECT_COMPOSITE].reset();

  if (m_pShader[ShaderMethodGLES::SM_STENCIL_ROUNDED_MASK])
    m_pShader[ShaderMethodGLES::SM_STENCIL_ROUNDED_MASK]->Free();
  m_pShader[ShaderMethodGLES::SM_STENCIL_ROUNDED_MASK].reset();

  if (m_roundMaskVbo != 0)
  {
    glDeleteBuffers(1, &m_roundMaskVbo);
    m_roundMaskVbo = 0;
  }

  // Free offscreen group resources
  if (m_groupFbo != 0)
  {
    glDeleteFramebuffers(1, &m_groupFbo);
    m_groupFbo = 0;
  }
  if (m_groupTex != 0)
  {
    glDeleteTextures(1, &m_groupTex);
    m_groupTex = 0;
  }
  m_groupW = 0;
  m_groupH = 0;
  m_groupActive = false;
}

bool CRenderSystemGLES::BeginStencilClip(const CRect& rectScreenTL, float radiusGui)
{
  // Single-level only for now.
  if (m_stencilRef != 0)
    return false;

  if (!HasStencilBufferForCurrentDrawTarget())
    return false;

  // Viewport in framebuffer pixels (x,y,w,h)
  GLint vp[4] = {0, 0, 0, 0};
  glGetIntegerv(GL_VIEWPORT, vp);
  const float vpW = static_cast<float>(vp[2]);
  const float vpH = static_cast<float>(vp[3]);
  if (vpW <= 0.0f || vpH <= 0.0f)
    return false;

  // Base GUI size in logical coords (no transforms). This is the key: radius should not scale with
  // the current transform matrix, only with GUI->framebuffer scaling.
  CGraphicContext& gfx = CServiceBroker::GetWinSystem()->GetGfxContext();
  const float guiW = static_cast<float>(gfx.GetWidth());
  const float guiH = static_cast<float>(gfx.GetHeight());
  if (guiW <= 0.0f || guiH <= 0.0f)
    return false;

  // Convert final screen coords (top-left) -> framebuffer pixels (bottom-left).
  // At this point, rectScreenTL is already scaled by GraphicContext::ScaleFinalCoords().
  CRect rectFbBL(rectScreenTL.x1,
                 vpH - rectScreenTL.y2,
                 rectScreenTL.x2,
                 vpH - rectScreenTL.y1);

  if (rectFbBL.x2 < rectFbBL.x1)
    std::swap(rectFbBL.x1, rectFbBL.x2);
  if (rectFbBL.y2 < rectFbBL.y1)
    std::swap(rectFbBL.y1, rectFbBL.y2);

  if (rectFbBL.IsEmpty())
    return false;

  // Radius: GUI logical -> framebuffer pixels (ignore current transforms).
  const float sx = vpW / guiW;
  const float sy = vpH / guiH;
  const float s  = std::max(sx, sy);

  const float maxR = std::min(rectFbBL.Width(), rectFbBL.Height()) * 0.5f;
  const float rFb  = std::max(0.0f, std::min(radiusGui * s, maxR));

  // Grab the rounded-mask shader (already compiled in InitialiseShaders)
  auto* shader = m_pShader[ShaderMethodGLES::SM_STENCIL_ROUNDED_MASK].get();
  if (!shader)
    return false;

  const GLuint prog = shader->ProgramHandle();
  if (prog == 0)
    return false;

  // ---- Save minimal GL state we touch ----
  GLboolean prevColorMask[4] = {GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE};
  glGetBooleanv(GL_COLOR_WRITEMASK, prevColorMask);

  GLboolean prevDepthMask = GL_TRUE;
  glGetBooleanv(GL_DEPTH_WRITEMASK, &prevDepthMask);

  const GLboolean depthTestWasEnabled = glIsEnabled(GL_DEPTH_TEST);
  const GLboolean blendWasEnabled     = glIsEnabled(GL_BLEND);

  GLint prevArrayBuf = 0;
  glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &prevArrayBuf);

  GLint prevProgram = 0;
  glGetIntegerv(GL_CURRENT_PROGRAM, &prevProgram);

  // ---- Prepare stencil ----
  glEnable(GL_STENCIL_TEST);

  // Clear stencil fully to avoid stale ref values outside the clip rect.
  glStencilMask(0xFF);
  glClearStencil(0);
  glClear(GL_STENCIL_BUFFER_BIT);

  constexpr GLint kRef = 1;
  glStencilFunc(GL_ALWAYS, kRef, 0xFF);
  glStencilOp(GL_KEEP, GL_KEEP, GL_REPLACE);

  // Mask draw should not affect color/depth; avoid depth/blend interference.
  glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);
  glDepthMask(GL_FALSE);
  if (depthTestWasEnabled)
    glDisable(GL_DEPTH_TEST);
  if (blendWasEnabled)
    glDisable(GL_BLEND);

  // ---- Bind program directly (do NOT call EnableGUIShader) ----
  glUseProgram(prog);

  // Provide uniforms in framebuffer pixel space (bottom-left origin).
  if (m_maskRectLoc >= 0)
    glUniform4f(m_maskRectLoc, rectFbBL.x1, rectFbBL.y1, rectFbBL.x2, rectFbBL.y2);
  if (m_maskRadiusLoc >= 0)
    glUniform1f(m_maskRadiusLoc, rFb);

  // Force identity matrix so we can supply vertices in NDC (-1..1).
  const GLint matrixLoc = glGetUniformLocation(prog, "m_matrix");
  if (matrixLoc >= 0)
  {
    const GLfloat I[16] = {
        1, 0, 0, 0,
        0, 1, 0, 0,
        0, 0, 1, 0,
        0, 0, 0, 1
    };
    glUniformMatrix4fv(matrixLoc, 1, GL_FALSE, I);
  }

  // ---- Build rounded-rect geometry in framebuffer pixels, convert to NDC ----
  auto toNdcX = [&](float xPx) -> float { return (xPx / vpW) * 2.0f - 1.0f; };
  auto toNdcY = [&](float yPx) -> float { return (yPx / vpH) * 2.0f - 1.0f; };

  const float rx = rFb;
  const float ry = rFb;

  const float px1 = rectFbBL.x1;
  const float py1 = rectFbBL.y1;
  const float px2 = rectFbBL.x2;
  const float py2 = rectFbBL.y2;

  // Corner centers (in framebuffer pixels)
  const float cblx = px1 + rx, cbly = py1 + ry;
  const float cbrx = px2 - rx, cbry = py1 + ry;
  const float ctrx = px2 - rx, ctry = py2 - ry;
  const float ctlx = px1 + rx, ctly = py2 - ry;

  // If radius is 0, it's just a rectangle fan.
  const int kSeg = 10; // segments per corner (tweak if you want smoother)
  std::vector<GLfloat> verts;
  verts.reserve(2 * (1 + 4 * (kSeg + 1) + 1));

  // Fan center (NDC)
  const float cx = 0.5f * (px1 + px2);
  const float cy = 0.5f * (py1 + py2);
  verts.push_back(toNdcX(cx));
  verts.push_back(toNdcY(cy));

  auto pushArc = [&](float ccx, float ccy, float a0, float a1)
  {
    // angles in radians
    for (int i = 0; i <= kSeg; ++i)
    {
      const float t = static_cast<float>(i) / static_cast<float>(kSeg);
      const float a = a0 + (a1 - a0) * t;
      const float x = ccx + std::cos(a) * rx;
      const float y = ccy + std::sin(a) * ry;
      verts.push_back(toNdcX(x));
      verts.push_back(toNdcY(y));
    }
  };

  if (rFb > 0.0f)
  {
    // Perimeter clockwise starting at bottom-left corner going to bottom-right etc.
    // Bottom-left arc: 180° -> 270°
    pushArc(cblx, cbly, static_cast<float>(M_PI), static_cast<float>(1.5 * M_PI));
    // Bottom-right arc: 270° -> 360°
    pushArc(cbrx, cbry, static_cast<float>(1.5 * M_PI), static_cast<float>(2.0 * M_PI));
    // Top-right arc: 0° -> 90°
    pushArc(ctrx, ctry, 0.0f, static_cast<float>(0.5 * M_PI));
    // Top-left arc: 90° -> 180°
    pushArc(ctlx, ctly, static_cast<float>(0.5 * M_PI), static_cast<float>(M_PI));
  }
  else
  {
    // Rectangle perimeter (clockwise)
    verts.push_back(toNdcX(px1)); verts.push_back(toNdcY(py1));
    verts.push_back(toNdcX(px2)); verts.push_back(toNdcY(py1));
    verts.push_back(toNdcX(px2)); verts.push_back(toNdcY(py2));
    verts.push_back(toNdcX(px1)); verts.push_back(toNdcY(py2));
  }

  // Close the loop by repeating the first perimeter point (after center)
  if (verts.size() >= 4)
  {
    verts.push_back(verts[2]);
    verts.push_back(verts[3]);
  }

  // Upload + draw
  if (m_roundMaskVbo == 0)
    glGenBuffers(1, &m_roundMaskVbo);

  glBindBuffer(GL_ARRAY_BUFFER, m_roundMaskVbo);
  glBufferData(GL_ARRAY_BUFFER, verts.size() * sizeof(GLfloat), verts.data(), GL_STREAM_DRAW);

  bool drew = false;
  if (m_roundMaskPosLoc >= 0)
  {
    glEnableVertexAttribArray(m_roundMaskPosLoc);
    glVertexAttribPointer(m_roundMaskPosLoc, 2, GL_FLOAT, GL_FALSE, 0, nullptr);
    glDrawArrays(GL_TRIANGLE_FAN, 0, static_cast<GLsizei>(verts.size() / 2));
    glDisableVertexAttribArray(m_roundMaskPosLoc);
    drew = true;
  }

  // ---- Restore state ----
  glBindBuffer(GL_ARRAY_BUFFER, prevArrayBuf);
  glUseProgram(prevProgram);

  glColorMask(prevColorMask[0], prevColorMask[1], prevColorMask[2], prevColorMask[3]);
  glDepthMask(prevDepthMask);

  if (depthTestWasEnabled)
    glEnable(GL_DEPTH_TEST);
  if (blendWasEnabled)
    glEnable(GL_BLEND);

  if (!drew)
  {
    glDisable(GL_STENCIL_TEST);
    return false;
  }

  // Switch to clip mode: only draw where stencil == ref
  glStencilMask(0x00);
  glStencilFunc(GL_EQUAL, kRef, 0xFF);
  glStencilOp(GL_KEEP, GL_KEEP, GL_KEEP);

  m_stencilRef = static_cast<uint8_t>(kRef);
  return true;
}

void CRenderSystemGLES::EndStencilClip()
{
  if (m_stencilRef == 0)
    return;

  m_stencilRef = 0;
  glDisable(GL_STENCIL_TEST);
}

bool CRenderSystemGLES::EnsureGroupFbo(int w, int h)
{
  if (w <= 0 || h <= 0)
    return false;

  if (m_groupFbo != 0 && m_groupTex != 0 && m_groupW == w && m_groupH == h)
    return true;

  if (m_groupFbo != 0)
  {
    glDeleteFramebuffers(1, &m_groupFbo);
    m_groupFbo = 0;
  }
  if (m_groupTex != 0)
  {
    glDeleteTextures(1, &m_groupTex);
    m_groupTex = 0;
  }

  glGenTextures(1, &m_groupTex);
  glBindTexture(GL_TEXTURE_2D, m_groupTex);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);

  glGenFramebuffers(1, &m_groupFbo);
  glBindFramebuffer(GL_FRAMEBUFFER, m_groupFbo);
  glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, m_groupTex, 0);

  const GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
  glBindFramebuffer(GL_FRAMEBUFFER, 0);
  if (status != GL_FRAMEBUFFER_COMPLETE)
  {
    CLog::Log(LOGERROR, "GLES: EnsureGroupFbo failed: {}x{} status=0x{:04x}",
              w, h, static_cast<unsigned int>(status));
    glDeleteFramebuffers(1, &m_groupFbo);
    glDeleteTextures(1, &m_groupTex);
    m_groupFbo = 0;
    m_groupTex = 0;
    return false;
  }

  m_groupW = w;
  m_groupH = h;
  CLog::Log(LOGINFO, "GLES: EnsureGroupFbo OK: {}x{} fbo={} tex={}", w, h, m_groupFbo, m_groupTex);
  return true;
}

bool CRenderSystemGLES::BeginOffscreenRoundedGroup(const CRect& rectScreenTL, float radiusPx)
{
  if (m_groupActive)
    return false;

  GLint vp[4] = {0, 0, 0, 0};
  glGetIntegerv(GL_VIEWPORT, vp);
  const int vpW = vp[2];
  const int vpH = vp[3];
  if (vpW <= 0 || vpH <= 0)
    return false;

  if (!EnsureGroupFbo(vpW, vpH))
    return false;

  glGetIntegerv(GL_FRAMEBUFFER_BINDING, &m_prevFbo);
  std::copy(std::begin(vp), std::end(vp), std::begin(m_prevViewport));

  glBindFramebuffer(GL_FRAMEBUFFER, m_groupFbo);
  glViewport(0, 0, vpW, vpH);

  glDisable(GL_SCISSOR_TEST);
  glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
  glClearColor(0.f, 0.f, 0.f, 0.f);
  glClear(GL_COLOR_BUFFER_BIT);

  m_offscreenRect = rectScreenTL;
  m_offscreenRadius = radiusPx;
  m_groupActive = true;
  return true;
}

void CRenderSystemGLES::EndOffscreenRoundedGroup()
{
  if (!m_groupActive)
    return;

  glBindFramebuffer(GL_FRAMEBUFFER, m_prevFbo);
  glViewport(m_prevViewport[0], m_prevViewport[1], m_prevViewport[2], m_prevViewport[3]);

  const float vpX = static_cast<float>(m_prevViewport[0]);
  const float vpY = static_cast<float>(m_prevViewport[1]);
  const float vpW = static_cast<float>(m_prevViewport[2]);
  const float vpH = static_cast<float>(m_prevViewport[3]);

  // rectScreenTL -> framebuffer bottom-left (with viewport offsets)
  CRect rectFbBL(m_offscreenRect.x1 + vpX,
                 vpY + (vpH - m_offscreenRect.y2),
                 m_offscreenRect.x2 + vpX,
                 vpY + (vpH - m_offscreenRect.y1));

  if (rectFbBL.x2 < rectFbBL.x1)
    std::swap(rectFbBL.x1, rectFbBL.x2);
  if (rectFbBL.y2 < rectFbBL.y1)
    std::swap(rectFbBL.y1, rectFbBL.y2);

  const float maxR = std::min(rectFbBL.Width(), rectFbBL.Height()) * 0.5f;
  const float rFb = std::max(0.0f, std::min(m_offscreenRadius, maxR));

  auto* shader = m_pShader[ShaderMethodGLES::SM_STENCIL_ROUNDED_MASK].get();
  if (!shader || shader->ProgramHandle() == 0 || m_groupTex == 0)
  {
    CLog::Log(LOGERROR,
              "GLES: EndOffscreenRoundedGroup abort: shader/prog/tex missing (shader={} prog={} tex={})",
              shader != nullptr, shader ? shader->ProgramHandle() : 0u, m_groupTex);
    m_groupActive = false;
    return;
  }

  const GLuint prog = shader->ProgramHandle();

  glEnable(GL_SCISSOR_TEST);
  glScissor(static_cast<GLint>(std::floor(rectFbBL.x1)),
            static_cast<GLint>(std::floor(rectFbBL.y1)),
            std::max(1, static_cast<GLint>(std::ceil(rectFbBL.Width()))),
            std::max(1, static_cast<GLint>(std::ceil(rectFbBL.Height()))));

  glEnable(GL_BLEND);
  glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

  glActiveTexture(GL_TEXTURE0);
  glBindTexture(GL_TEXTURE_2D, m_groupTex);

  glUseProgram(prog);

  if (m_maskSamplerLoc >= 0)
    glUniform1i(m_maskSamplerLoc, 0);
  if (m_maskViewportLoc >= 0)
    glUniform2f(m_maskViewportLoc, vpW, vpH);
  if (m_maskRectLoc >= 0)
    glUniform4f(m_maskRectLoc, rectFbBL.x1, rectFbBL.y1, rectFbBL.x2, rectFbBL.y2);
  if (m_maskRadiusLoc >= 0)
    glUniform1f(m_maskRadiusLoc, rFb);
  if (m_maskAAWidthLoc >= 0)
    glUniform1f(m_maskAAWidthLoc, 1.0f);

  // Identity matrix for NDC quad
  const GLint matrixLoc = glGetUniformLocation(prog, "m_matrix");
  if (matrixLoc >= 0)
  {
    const GLfloat I[16] = {
        1, 0, 0, 0,
        0, 1, 0, 0,
        0, 0, 1, 0,
        0, 0, 0, 1
    };
    glUniformMatrix4fv(matrixLoc, 1, GL_FALSE, I);
  }

  const GLfloat verts[8] = {
      -1.0f, -1.0f,
       1.0f, -1.0f,
      -1.0f,  1.0f,
       1.0f,  1.0f
  };

  if (m_roundMaskVbo == 0)
    glGenBuffers(1, &m_roundMaskVbo);

  glBindBuffer(GL_ARRAY_BUFFER, m_roundMaskVbo);
  glBufferData(GL_ARRAY_BUFFER, sizeof(verts), verts, GL_STREAM_DRAW);

  const GLint posLoc = (m_maskPosLoc >= 0) ? m_maskPosLoc : m_roundMaskPosLoc;
  if (posLoc >= 0)
  {
    glEnableVertexAttribArray(posLoc);
    glVertexAttribPointer(posLoc, 2, GL_FLOAT, GL_FALSE, 0, nullptr);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    glDisableVertexAttribArray(posLoc);
  }

  glDisable(GL_SCISSOR_TEST);

  m_groupActive = false;
}

void CRenderSystemGLES::EnableGUIShader(ShaderMethodGLES method)
{
  m_method = method;
  if (m_pShader[m_method])
  {
    m_pShader[m_method]->Enable();
  }
  else
  {
    CLog::Log(LOGERROR, "Invalid GUI Shader selected - {}", method);
  }
}

void CRenderSystemGLES::DisableGUIShader()
{
  if (m_pShader[m_method])
  {
    m_pShader[m_method]->Disable();
  }
  m_method = ShaderMethodGLES::SM_DEFAULT;
}

GLint CRenderSystemGLES::GUIShaderGetPos()
{
  if (m_pShader[m_method])
    return m_pShader[m_method]->GetPosLoc();

  return -1;
}

GLint CRenderSystemGLES::GUIShaderGetCol()
{
  if (m_pShader[m_method])
    return m_pShader[m_method]->GetColLoc();

  return -1;
}

GLint CRenderSystemGLES::GUIShaderGetCoord0()
{
  if (m_pShader[m_method])
    return m_pShader[m_method]->GetCord0Loc();

  return -1;
}

GLint CRenderSystemGLES::GUIShaderGetCoord1()
{
  if (m_pShader[m_method])
    return m_pShader[m_method]->GetCord1Loc();

  return -1;
}

GLint CRenderSystemGLES::GUIShaderGetDepth()
{
  if (m_pShader[m_method])
    return m_pShader[m_method]->GetDepthLoc();

  return -1;
}

GLint CRenderSystemGLES::GUIShaderGetUniCol()
{
  if (m_pShader[m_method])
    return m_pShader[m_method]->GetUniColLoc();

  return -1;
}

GLint CRenderSystemGLES::GUIShaderGetCoord0Matrix()
{
  if (m_pShader[m_method])
    return m_pShader[m_method]->GetCoord0MatrixLoc();

  return -1;
}

GLint CRenderSystemGLES::GUIShaderGetField()
{
  if (m_pShader[m_method])
    return m_pShader[m_method]->GetFieldLoc();

  return -1;
}

GLint CRenderSystemGLES::GUIShaderGetStep()
{
  if (m_pShader[m_method])
    return m_pShader[m_method]->GetStepLoc();

  return -1;
}

GLint CRenderSystemGLES::GUIShaderGetContrast()
{
  if (m_pShader[m_method])
    return m_pShader[m_method]->GetContrastLoc();

  return -1;
}

GLint CRenderSystemGLES::GUIShaderGetBrightness()
{
  if (m_pShader[m_method])
    return m_pShader[m_method]->GetBrightnessLoc();

  return -1;
}

bool CRenderSystemGLES::SupportsStereo(RENDER_STEREO_MODE mode) const
{
  return CRenderSystemBase::SupportsStereo(mode);
}

GLint CRenderSystemGLES::GUIShaderGetModel()
{
  if (m_pShader[m_method])
    return m_pShader[m_method]->GetModelLoc();

  return -1;
}

GLint CRenderSystemGLES::GUIShaderGetMatrix()
{
  if (m_pShader[m_method])
    return m_pShader[m_method]->GetMatrixLoc();

  return -1;
}

GLint CRenderSystemGLES::GUIShaderGetClip()
{
  if (m_pShader[m_method])
    return m_pShader[m_method]->GetShaderClipLoc();

  return -1;
}

GLint CRenderSystemGLES::GUIShaderGetCoordStep()
{
  if (m_pShader[m_method])
    return m_pShader[m_method]->GetShaderCoordStepLoc();

  return -1;
}

std::string CRenderSystemGLES::GetShaderPath(const std::string& filename)
{
  std::string path = "GLES/2.0/";

  if (m_RenderVersionMajor >= 3 && m_RenderVersionMinor >= 1)
  {
    std::string file = "special://xbmc/system/shaders/GLES/3.1/" + filename;
    const CURL pathToUrl(file);
    if (CFileUtils::Exists(pathToUrl.Get()))
      return "GLES/3.1/";
  }

  return path;
}
