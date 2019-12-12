
uniform sampler2D gpFillTexture;
uniform sampler2D gpStrokeTexture;

in vec4 finalColorMul;
in vec4 finalColorAdd;
in vec2 finalUvs;
flat in int matFlag;

out vec4 fragColor;

float length_squared(vec2 v)
{
  return dot(v, v);
}

void main()
{
  vec4 col;
  if (GP_FLAG_TEST(matFlag, GP_STROKE_TEXTURE_USE)) {
    bool premul = GP_FLAG_TEST(matFlag, GP_STROKE_TEXTURE_PREMUL);
    col = texture_read_as_srgb(gpStrokeTexture, premul, finalUvs);
  }
  else if (GP_FLAG_TEST(matFlag, GP_FILL_TEXTURE_USE)) {
    bool use_clip = GP_FLAG_TEST(matFlag, GP_FILL_TEXTURE_CLIP);
    vec2 uvs = (use_clip) ? clamp(finalUvs, 0.0, 1.0) : finalUvs;
    bool premul = GP_FLAG_TEST(matFlag, GP_FILL_TEXTURE_PREMUL);
    col = texture_read_as_srgb(gpFillTexture, premul, uvs);
  }
  else /* SOLID */ {
    col = vec4(1.0);
  }
  col.rgb *= col.a;
  /* Composite all other colors on top of texture color.
   * Everything is premult by col.a to have the stencil effect. */
  fragColor = col * finalColorMul + col.a * finalColorAdd;

  if (GP_FLAG_TEST(matFlag, GP_STROKE_DOTS)) {
    const float rad_sqr_inv = 1.0 / 0.25;
    float dist = 1.0 - rad_sqr_inv * length_squared(finalUvs - 0.5);
    fragColor *= clamp(dist, 0.0, 1.0);
  }

  if (fragColor.a < 0.001) {
    discard;
  }
}
