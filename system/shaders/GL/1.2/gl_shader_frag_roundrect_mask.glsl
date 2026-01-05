#version 150

in vec2 v_pos;

uniform vec4 m_maskRect; // x1,y1,x2,y2 in local GUI coords
uniform float m_radius;  // radius in local units (we convert from pixels in C++)

out vec4 fragColor;

float sdRoundRect(vec2 p, vec2 b, float r)
{
  // Signed distance to rounded rectangle centered at origin.
  // b = half-size minus radius.
  vec2 q = abs(p) - b;
  return length(max(q, 0.0)) + min(max(q.x, q.y), 0.0) - r;
}

void main()
{
  vec2 center = 0.5 * (m_maskRect.xy + m_maskRect.zw);
  vec2 halfSize = 0.5 * (m_maskRect.zw - m_maskRect.xy);

  float r = clamp(m_radius, 0.0, min(halfSize.x, halfSize.y));
  vec2 b = halfSize - vec2(r);

  float d = sdRoundRect(v_pos - center, b, r);
  if (d > 0.0)
    discard;

  // Color writes are disabled while drawing the mask, so this doesn't matter.
  fragColor = vec4(1.0);
}
