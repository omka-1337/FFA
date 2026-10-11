// The Emitters' sprites (world/Particles.h), drawn after what blends in the
// level: each a quad facing the camera, or along its velocity, or flat about
// ProjectionNormal, its frame of the texture's subdivisions, its colour and
// fade, blended by the emitter's DrawStyle. The emitters go far to near;
// their particles in the order they are kept.
#include <GLES2/gl2.h>

#include <algorithm>
#include <cmath>

#include "render/LevelRender.h"
#include "world/Particles.h"

namespace ffa {

namespace {

const char* kParticleVertex = R"(
attribute vec3 aPos;
attribute vec2 aUv;
attribute vec4 aColor;
uniform mat4 uMvp;
varying vec2 vUv;
varying vec4 vColor;
void main() {
    vUv = aUv;
    vColor = aColor;
    gl_Position = uMvp * vec4(aPos, 1.0);
}
)";

// uPremultiply: the colour times its alpha, for the styles that add or
// darken by the colour alone
const char* kParticleFragment = R"(
precision mediump float;
uniform sampler2D uTex;
uniform float uPremultiply;
uniform float uCut;
varying vec2 vUv;
varying vec4 vColor;
void main() {
    vec4 c = texture2D(uTex, vUv) * vColor;
    if (c.a < uCut) discard;
    c.rgb = mix(c.rgb, c.rgb * c.a, uPremultiply);
    gl_FragColor = c;
}
)";

unsigned compileParticle(GLenum type, const char* src) {
    unsigned s = glCreateShader(type);
    glShaderSource(s, 1, &src, nullptr);
    glCompileShader(s);
    return s;
}

// EParticleDrawStyle as GL blending: Regular, AlphaBlend, Modulated,
// Translucent, AlphaModulate, Darken, Brighten.
void blendFor(int style, float& premultiply, float& cut) {
    premultiply = 0;
    cut = 0.004f;
    glEnable(GL_BLEND);
    switch (style) {
    case 0:
        glDisable(GL_BLEND);
        cut = 0.5f;
        break;
    case 1: glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA); break;
    case 2: glBlendFunc(GL_DST_COLOR, GL_SRC_COLOR); break;
    case 4:
        glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
        premultiply = 1;
        break;
    case 5:
        glBlendFunc(GL_ZERO, GL_ONE_MINUS_SRC_COLOR);
        premultiply = 1;
        break;
    case 6: glBlendFunc(GL_SRC_ALPHA, GL_ONE); break;
    default:
        glBlendFunc(GL_ONE, GL_ONE);
        premultiply = 1;
        break;
    }
}

Vec3 normalized(Vec3 v) {
    float l = length(v);
    return l > 1e-6f ? v * (1 / l) : Vec3{0, 0, 1};
}

}  // namespace

void LevelRender::drawParticles(const float mvp[16], Vec3 eye, const float ax[3][3]) {
    const std::vector<ParticleDraw>& draws = particleDraws(*session_.world);
    if (draws.empty()) return;
    if (!particleProgram_) {
        particleProgram_ = glCreateProgram();
        glAttachShader(particleProgram_, compileParticle(GL_VERTEX_SHADER, kParticleVertex));
        glAttachShader(particleProgram_, compileParticle(GL_FRAGMENT_SHADER, kParticleFragment));
        glBindAttribLocation(particleProgram_, 0, "aPos");
        glBindAttribLocation(particleProgram_, 1, "aUv");
        glBindAttribLocation(particleProgram_, 2, "aColor");
        glLinkProgram(particleProgram_);
    }
    World& w = *session_.world;
    std::vector<std::pair<float, const ParticleDraw*>> order;
    for (const ParticleDraw& d : draws) {
        Vec3 at;
        w.vm.unvector(w.var(d.actor, "Location"), at.x, at.y, at.z);
        Vec3 dd = at - eye;
        order.push_back({dot(dd, dd), &d});
    }
    std::stable_sort(order.begin(), order.end(), [](const auto& a, const auto& b) { return a.first > b.first; });

    glUseProgram(particleProgram_);
    glUniformMatrix4fv(glGetUniformLocation(particleProgram_, "uMvp"), 1, GL_FALSE, mvp);
    glUniform1i(glGetUniformLocation(particleProgram_, "uTex"), 0);
    GLint uPre = glGetUniformLocation(particleProgram_, "uPremultiply"), uCut = glGetUniformLocation(particleProgram_, "uCut");
    glActiveTexture(GL_TEXTURE0);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    for (int a = 3; a < 6; ++a) glDisableVertexAttribArray(GLuint(a));
    for (int a = 0; a < 3; ++a) glEnableVertexAttribArray(GLuint(a));
    const Vec3 right{ax[1][0], ax[1][1], ax[1][2]}, up{ax[2][0], ax[2][1], ax[2][2]}, ahead{ax[0][0], ax[0][1], ax[0][2]};
    std::vector<float> v;
    for (auto& [dist, dp] : order) {
        const ParticleDraw& d = *dp;
        int tw = 0, th = 0;
        unsigned tex = d.texture ? hudTexture(d.texture, tw, th) : 0;
        if (!tex) continue;
        v.clear();
        for (const ParticleSprite& s : d.sprites) {
            // the quad's two axes, by UseDirectionAs
            Vec3 r = right, u = up;
            if (d.facing == 1) {
                u = s.up;
                r = normalized(cross(ahead, u));
            } else if (d.facing == 4) {
                Vec3 n = normalized(d.normal);
                r = normalized(cross(n, std::fabs(n.z) < 0.9f ? Vec3{0, 0, 1} : Vec3{1, 0, 0}));
                u = cross(n, r);
            }
            float c = std::cos(s.spin), sn = std::sin(s.spin);
            Vec3 rr = r * c + u * sn, uu = u * c - r * sn;
            Vec3 hx = rr * s.width, hy = uu * s.height;
            int col = s.frame % d.columns, row = s.frame / d.columns % d.rows;
            float u0 = float(col) / float(d.columns), u1 = float(col + 1) / float(d.columns);
            float v0 = float(row) / float(d.rows), v1 = float(row + 1) / float(d.rows);
            const Vec3 corner[4] = {s.at - hx + hy, s.at + hx + hy, s.at + hx - hy, s.at - hx - hy};
            const float uv[4][2] = {{u0, v0}, {u1, v0}, {u1, v1}, {u0, v1}};
            for (int k : {0, 1, 2, 0, 2, 3}) {
                v.insert(v.end(), {corner[k].x, corner[k].y, corner[k].z, uv[k][0], uv[k][1], s.color[0], s.color[1],
                                   s.color[2], s.color[3]});
            }
        }
        if (v.empty()) continue;
        float pre, cut;
        blendFor(d.style, pre, cut);
        glUniform1f(uPre, pre);
        glUniform1f(uCut, cut);
        glDepthMask(d.zWrite ? GL_TRUE : GL_FALSE);
        if (d.zTest)
            glEnable(GL_DEPTH_TEST);
        else
            glDisable(GL_DEPTH_TEST);
        glBindTexture(GL_TEXTURE_2D, tex);
        const GLsizei stride = 9 * sizeof(float);
        glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, stride, v.data());
        glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, stride, v.data() + 3);
        glVertexAttribPointer(2, 4, GL_FLOAT, GL_FALSE, stride, v.data() + 5);
        glDrawArrays(GL_TRIANGLES, 0, GLsizei(v.size() / 9));
        particlesDrawn += v.size() / 54;
    }
    glEnable(GL_DEPTH_TEST);
    glDepthMask(GL_TRUE);
    glDisable(GL_BLEND);
}

}  // namespace ffa
