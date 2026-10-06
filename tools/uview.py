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
from ulevel import level_model, polygons, MeshLibrary, static_mesh_instances

# Unreal (x, y, z) to three.js (x, z, y), in metres. One swap flips handedness
# and moves Z up to Y up; Unreal units are roughly centimetres.
def conv(v):
    return (v[0] / 100.0, v[2] / 100.0, v[1] / 100.0)


def sub(a, b):
    return (a[0] - b[0], a[1] - b[1], a[2] - b[2])


def cross(a, b):
    return (a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2],
            a[0] * b[1] - a[1] * b[0])

THREE = 'https://cdnjs.cloudflare.com/ajax/libs/three.js/r128/three.min.js'
ORBIT = 'https://cdn.jsdelivr.net/npm/three@0.128.0/examples/js/controls/OrbitControls.js'


def tint(name):
    """A stable pastel colour per material name, until real textures exist."""
    h = (zlib.crc32(name.encode()) % 360) / 360.0
    return colorsys.hsv_to_rgb(h, 0.35, 1.0)


def build_bsp(pkg):
    """BSP polygons, each triangle wound so it faces the way its node's plane
    does. Drawn front side only, a level can then be looked into from outside,
    since its faces point into the playable space."""
    m = level_model(pkg)
    pos, col, idx = [], [], []
    mats = collections.Counter()
    for pts, mat, flags, nrm in polygons(m):
        base = len(pos) // 3
        c = tint(mat)
        P = [conv(p) for p in pts]
        N = (nrm[0], nrm[2], nrm[1])
        for p in P:
            pos += p
            col += [int(c[0] * 255), int(c[1] * 255), int(c[2] * 255)]
        for i in range(1, len(P) - 1):
            g = cross(sub(P[i], P[0]), sub(P[i + 1], P[0]))
            if g[0] * N[0] + g[1] * N[1] + g[2] * N[2] >= 0:
                idx += [base, base + i, base + i + 1]
            else:
                idx += [base, base + i + 1, base + i]
        mats[mat] += 1
    return pos, col, idx, mats


def build_meshes(pkg, path, lib):
    """Unique static meshes once each, plus one transform per placed actor."""
    uniq, inst = {}, collections.defaultdict(list)
    for key, mesh, cols, loc, name in static_mesh_instances(pkg, path, lib):
        if key not in uniq:
            pos = []
            for v in mesh.verts:
                pos += conv(v[:3])
            uniq[key] = (mesh.name, pos, list(mesh.indices))
        # three.js matrix = P A P with P the Y/Z swap; column major for three.js
        p = (0, 2, 1)
        A3 = [[cols[p[c]][p[r]] for c in range(3)] for r in range(3)]
        t = conv(loc)
        inst[key].append([A3[0][0], A3[1][0], A3[2][0], 0.0,
                          A3[0][1], A3[1][1], A3[2][1], 0.0,
                          A3[0][2], A3[1][2], A3[2][2], 0.0,
                          t[0], t[1], t[2], 1.0])
    return uniq, inst


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
<div id="info"><b>%(title)s</b><br>%(stats)s<br>drag rotate · right drag pan · wheel zoom<br>
B: BSP walls one sided / both sides · M: static meshes on / off<br><br>%(legend)s</div>
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
const bspMat=new THREE.MeshLambertMaterial({vertexColors:true,side:THREE.FrontSide});
scene.add(new THREE.Mesh(g,bspMat));
const meshes=new THREE.Group(); scene.add(meshes);
for (const m of %(meshes)s){
  const mg=new THREE.BufferGeometry();
  mg.setAttribute('position',new THREE.BufferAttribute(dec(m.pos,Float32Array),3));
  mg.setIndex(new THREE.BufferAttribute(dec(m.idx,Uint32Array),1));
  mg.computeVertexNormals();
  const mat=dec(m.mat,Float32Array), n=mat.length/16;
  const im=new THREE.InstancedMesh(mg,new THREE.MeshLambertMaterial({color:m.col,side:THREE.DoubleSide}),n);
  const M=new THREE.Matrix4();
  for(let i=0;i<n;i++){M.fromArray(mat,i*16); im.setMatrixAt(i,M);}
  im.instanceMatrix.needsUpdate=true; meshes.add(im);
}
addEventListener('keydown',e=>{
  if(e.key==='b'||e.key==='B'){bspMat.side=bspMat.side===THREE.FrontSide?THREE.DoubleSide:THREE.FrontSide;bspMat.needsUpdate=true}
  if(e.key==='m'||e.key==='M'){meshes.visible=!meshes.visible}
});
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
    meshdir = argv[2] if len(argv) > 2 else os.path.join(
        os.path.dirname(os.path.dirname(os.path.abspath(src))), 'StaticMeshes')
    pkg = Package(src)
    pos, col, idx, mats = build_bsp(pkg)
    lib = MeshLibrary([meshdir])
    uniq, inst = build_meshes(pkg, src, lib)
    mesh_json = json.dumps([
        dict(pos=b64('f', uniq[k][1]), idx=b64('I', uniq[k][2]),
             mat=b64('f', [x for m in inst[k] for x in m]),
             col='#%02x%02x%02x' % tuple(int(min(1.0, v) * 255) for v in
                                          (tint(uniq[k][0])[0] * 1.15,
                                           tint(uniq[k][0])[1] * 0.75,
                                           tint(uniq[k][0])[2] * 0.6)))
        for k in uniq])
    ninst = sum(len(v) for v in inst.values())
    ntri = sum(len(uniq[k][2]) // 3 * len(inst[k]) for k in uniq)
    title = os.path.splitext(os.path.basename(src))[0]
    legend = '<br>'.join(
        '<span class="sw" style="background:rgb(%d,%d,%d)"></span>%s (%d)'
        % (tuple(int(v * 255) for v in tint(n)) + (n, k))
        for n, k in mats.most_common(12))
    html = PAGE % dict(title=title, three=THREE, orbit=ORBIT,
                       stats='BSP %d polygons, %d triangles<br>static meshes %d placed, %d unique, %d triangles'
                             % (sum(mats.values()), len(idx) // 3, ninst, len(uniq), ntri),
                       legend=legend, pos=b64('f', pos), col=b64('B', col), idx=b64('I', idx),
                       meshes=mesh_json)
    os.makedirs(os.path.dirname(os.path.abspath(out)), exist_ok=True)
    open(out, 'w').write(html)
    print('%s: BSP %d triangles, %d mesh instances (%d unique), %.1f MB -> %s'
          % (title, len(idx) // 3, ninst, len(uniq), len(html) / 1e6, out))


if __name__ == '__main__':
    main(sys.argv[1:])
