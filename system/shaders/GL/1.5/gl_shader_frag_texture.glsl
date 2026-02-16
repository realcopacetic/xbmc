/*
 *  Copyright (C) 2010-2024 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#version 150

uniform sampler2D m_samp0;
uniform vec4 m_unicol;
uniform vec4 m_shaderClip;        // x1,y1,x2,y2
uniform float m_shaderClipRadius; // uniform radius in pixels
in vec2 m_cord0;
in vec2 m_clipPos;
out vec4 fragColor;

float sdRoundRect(vec2 p, vec2 h, float r)
{
  vec2 q = abs(p) - (h - vec2(r));
  return length(max(q, 0.0)) - r;
}

float roundRectAlpha(vec2 pos, vec4 rect, float radius)
{
  if (radius <= 0.0)
    return 1.0;

  vec2 minp = rect.xy;
  vec2 maxp = rect.zw;
  vec2 c = (minp + maxp) * 0.5;
  vec2 h = (maxp - minp) * 0.5;
  vec2 d = pos - c;

  float r = min(radius, min(h.x, h.y));
  float dist = sdRoundRect(d, h, r);
  float aa = max(fwidth(dist), 1.0);
  return 1.0 - smoothstep(0.0, aa, dist);
}

// SM_TEXTURE shader
void main()
{
  fragColor = texture(m_samp0, m_cord0) * m_unicol;
  fragColor.a *= roundRectAlpha(m_clipPos, m_shaderClip, m_shaderClipRadius);
#if defined(KODI_LIMITED_RANGE)
  fragColor.rgb *= (235.0-16.0) / 255.0;
  fragColor.rgb += 16.0 / 255.0;
#endif
}
