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
uniform vec2 m_dimensions; 
uniform vec4 m_atlasBounds; // x=u_min, y=v_min, z=u_max, w=v_max
uniform float m_radius;

in vec2 m_cord0;
out vec4 fragColor;

float sdRoundedBox(vec2 p, vec2 b, float r)
{
  vec2 q = abs(p) - b + r;
  return min(max(q.x, q.y), 0.0) + length(max(q, 0.0)) - r;
}

void main() 
{
  vec4 color = texture(m_samp0, m_cord0) * m_unicol;

  // Prevent division by zero just in case
  vec2 atlasRange = max(m_atlasBounds.zw - m_atlasBounds.xy, vec2(0.0001));
  
  // MAGIC: Reconstruct a perfect 0.0 -> 1.0 coordinate for this specific image!
  vec2 localUV = (m_cord0 - m_atlasBounds.xy) / atlasRange;

  // Now we have perfect, rotation-proof local pixel coordinates
  vec2 localPos = localUV * m_dimensions; 
  vec2 halfSize = m_dimensions * 0.5;
  vec2 localCenter = halfSize;

  float safeRadius = min(m_radius, min(halfSize.x, halfSize.y));

  float dist = sdRoundedBox(localPos - localCenter, halfSize, safeRadius);
  float alpha = smoothstep(0.0, 1.0, -dist);

  if (alpha <= 0.0) discard;

  color.a *= alpha;
  fragColor = color;
}
