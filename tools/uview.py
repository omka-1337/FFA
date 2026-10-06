"""Write a level's geometry as a self contained HTML viewer.

Open the file in any browser and orbit with the mouse. The page loads three.js
from a CDN and carries the geometry inline, so nothing else is needed.

The output contains the game's own level geometry. It is for looking at your own
copy and must not be committed or published; `out/` is gitignored for that.

Unreal is left handed with Z up, three.js right handed with Y up. Swapping Y and
Z does both conversions at once, because a single axis swap flips handedness.
"""
import sys, os, json, base64, struct, zlib, colorsys, collections

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from upkg import Package
from ulevel import level_model, polygons

THREE = 'https://cdnjs.cloudflare.com/ajax/libs/three.js/r128/three.min.js'
ORBIT = 'https://cdn.jsdelivr.net/npm/three@0.128.0/examples/js/controls/OrbitControls.js'


def tint(name):
    """A stable pastel colour per material name, until real textures exist."""
    h = (zlib.crc32(name.encode()) % 360) / 360.0
    return colorsys.hsv_to_rgb(h, 0.35, 1.0)


def build(path):
    m = level_model(Package(path))
    pos, col, idx = [], [], []
    mats = collections.Counter()
    for pts, mat, flags in polygons(m):
        base = len(pos) // 3
        c = tint(mat)
        for x, y, z in pts:
            pos += [x / 100.0, z / 100.0, y / 100.0]
            col += [int(c[0] * 255), int(c[1] * 255), int(c[2] * 255)]
        for i in range(1, len(pts) - 1):
            idx += [base, base + i, base + i + 1]
        mats[mat] += 1
    return pos, col, idx, mats


def b64(fmt, data):
    return base64.b64encode(struct.pack('<%d%s' % (len(data), fmt), *data)).decode()


PAGE = """<!doctype html>
<html><head><meta charset="utf-8"><title>%(title)s</title>
<style>
html,body{margin:0;height:100%%;background:#18181f;overflow:hidden;font:13px system-ui,sans-serif;color:#ddd}
#info{position:fixed;top:10px;left:12px;background:rgba(0,0,0,.55);padding:8px 11px;border-radius:6px;line-height:1.5}
#info b{color:#fff}
.sw{display:inline-block;width:10px;height:10px;margin-right:6px;border-radius:2px;vertical-align:middle}
</style></head><body>
<div id="info"><b>%(title)s</b><br>%(stats)s<br>drag rotate · right drag pan · wheel zoom<br><br>%(legend)s</div>
<script src="%(three)s"></script>
<script src="%(orbit)s"></script>
<script>
function dec(s, T){const b=atob(s),u=new Uint8Array(b.length);for(let i=0;i<b.length;i++)u[i]=b.charCodeAt(i);return new T(u.buffer)}
const pos=dec("%(pos)s",Float32Array), col=dec("%(col)s",Uint8Array), idx=dec("%(idx)s",Uint32Array);
const g=new THREE.BufferGeometry();
g.setAttribute('position',new THREE.BufferAttribute(pos,3));
g.setAttribute('color',new THREE.BufferAttribute(col,3,true));
g.setIndex(new THREE.BufferAttribute(idx,1));
g.computeVertexNormals(); g.computeBoundingSphere();
const scene=new THREE.Scene();
scene.add(new THREE.Mesh(g,new THREE.MeshLambertMaterial({vertexColors:true,side:THREE.DoubleSide})));
scene.add(new THREE.HemisphereLight(0xffffff,0x404050,0.75));
const sun=new THREE.DirectionalLight(0xffffff,0.6); sun.position.set(0.4,1,0.3); scene.add(sun);
const R=g.boundingSphere.radius, C=g.boundingSphere.center;
const cam=new THREE.PerspectiveCamera(55,innerWidth/innerHeight,R/1000,R*20);
cam.position.set(C.x-R*0.8,C.y+R*0.9,C.z-R*0.8);
const ren=new THREE.WebGLRenderer({antialias:true}); ren.setPixelRatio(devicePixelRatio);
ren.setSize(innerWidth,innerHeight); document.body.appendChild(ren.domElement);
const ctl=new THREE.OrbitControls(cam,ren.domElement); ctl.target.copy(C); ctl.update();
addEventListener('resize',()=>{cam.aspect=innerWidth/innerHeight;cam.updateProjectionMatrix();ren.setSize(innerWidth,innerHeight)});
(function loop(){requestAnimationFrame(loop);ren.render(scene,cam)})();
</script></body></html>
"""


def main(argv):
    src, out = argv[0], argv[1]
    pos, col, idx, mats = build(src)
    title = os.path.splitext(os.path.basename(src))[0]
    legend = '<br>'.join(
        '<span class="sw" style="background:rgb(%d,%d,%d)"></span>%s (%d)'
        % (tuple(int(v * 255) for v in tint(n)) + (n, k))
        for n, k in mats.most_common(12))
    html = PAGE % dict(title=title, three=THREE, orbit=ORBIT,
                       stats='%d polygons, %d triangles' % (sum(mats.values()), len(idx) // 3),
                       legend=legend, pos=b64('f', pos), col=b64('B', col), idx=b64('I', idx))
    os.makedirs(os.path.dirname(os.path.abspath(out)), exist_ok=True)
    open(out, 'w').write(html)
    print('%s: %d triangles, %.1f MB -> %s' % (title, len(idx) // 3, len(html) / 1e6, out))


if __name__ == '__main__':
    main(sys.argv[1:])
