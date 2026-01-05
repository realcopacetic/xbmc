#version 150

in vec4 m_attrpos;
in vec4 m_attrcol;
in vec2 m_attrcord0;
in vec2 m_attrcord1;

uniform mat4 m_matrix;

out vec2 v_pos;

void main()
{
  // v_pos stays in local GUI coordinates so rotation is handled "for free"
  // by the transform matrices, while the SDF math remains stable.
  v_pos = m_attrpos.xy;
  gl_Position = m_matrix * vec4(m_attrpos.xy, 0.0, 1.0);
}
