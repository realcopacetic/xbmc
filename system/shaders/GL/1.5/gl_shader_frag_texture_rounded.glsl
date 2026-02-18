/*
 * Copyright (C) 2010-2024 Team Kodi
 * This file is part of Kodi - https://kodi.tv
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 * See LICENSES/README.md for more information.
 */

#version 150

uniform sampler2D m_samp0;
uniform vec4 m_unicol; 
uniform vec4 m_clipRect; 
uniform float m_radius;

in vec2 m_cord0;
in vec4 m_colour;
out vec4 fragColor;

float sdRoundedBox(vec2 p, vec2 b, float r)
{
  vec2 q = abs(p) - b + r;
  return min(max(q.x, q.y), 0.0) + length(max(q, 0.0)) - r;
}

void main() 
{
  vec4 color = texture(m_samp0, m_cord0) * m_unicol;

  vec2 pixelPos = gl_FragCoord.xy;

  vec4 snappedRect = floor(m_clipRect + 0.5); 
  vec2 rectCenter = snappedRect.xy + snappedRect.zw * 0.5;
  vec2 halfSize   = snappedRect.zw * 0.5;

  float safeRadius = min(m_radius, min(halfSize.x, halfSize.y));
  float dist = sdRoundedBox(pixelPos - rectCenter, halfSize, safeRadius);
  float alpha = smoothstep(0.0, 0.5, -dist);

  if (alpha <= 0.0)
  {
    discard;
  }

  color.a *= alpha;
  fragColor = color;
}
