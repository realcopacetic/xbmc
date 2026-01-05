#version 150

// Rect in framebuffer pixels, bottom-left origin: x1,y1,x2,y2
uniform vec4  m_maskRect;
uniform float m_radius; // radius in framebuffer pixels

out vec4 fragColor;

float sdRoundRect(vec2 p, vec2 b, float r)
{
  vec2 q = abs(p) - b;
  return length(max(q, 0.0)) + min(max(q.x, q.y), 0.0) - r;
}

void main()
{
  // gl_FragCoord is framebuffer pixels, bottom-left origin
  vec2 p = gl_FragCoord.xy;

  vec2 center   = 0.5 * (m_maskRect.xy + m_maskRect.zw);
  vec2 halfSize = 0.5 * (m_maskRect.zw - m_maskRect.xy);

  float r = clamp(m_radius, 0.0, min(halfSize.x, halfSize.y));
  vec2 b  = halfSize - vec2(r);

  float d = sdRoundRect(p - center, b, r);
  if (d > 0.0)
    discard;

  fragColor = vec4(1.0);
}
