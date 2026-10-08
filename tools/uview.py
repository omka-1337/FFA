"""Write a level's geometry as a self contained HTML viewer.

Open the file in any browser and fly through it. The page loads three.js from a
CDN and carries the geometry and the textures inline, so nothing else is needed.

The output contains the game's own level geometry. It is for looking at your own
copy and must not be committed or published; `out/` is gitignored for that.

Unreal is left handed with Z up, three.js right handed with Y up. Swapping Y and
Z does both conversions at once, because a single axis swap flips handedness.
"""
import sys, os, json, base64, struct, zlib, colorsys, collections

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from upkg import Package, R
from umap import Map
from ulevel import (level_model, polygons, MeshLibrary, static_mesh_instances,
                    ClassDefaults, skeletal_instances, skeletal_local_points,
                    import_path)
from uterrain import terrains
from umaterial import MaterialResolver, mesh_materials
from utexture import Texture, png_bytes


class TextureTable:
    """Unique textures of a page, each once, as PNG data URIs no larger than
    MAX_SIDE, with a flag for whether alpha is in use (foliage cards and the
    like are drawn with an alpha test)."""
    MAX_SIDE = 256

    def __init__(self, lib):
        self.lib, self.mr = lib, MaterialResolver(lib)
        self.ids, self.items, self.sizes = {}, [], {}

    def palette(self, pkg, ref):
        """A Palette held by another package, for paletted textures."""
        ch = import_path(pkg, ref)
        p = self.lib.package(self.lib.files[ch[0].lower()])
        return p, next(e for e in p.exports if e['name'].lower() == ch[-1].lower()
                       and p.classof(e) == 'Palette')

    def for_material(self, pkg, path, ref):
        """Texture id for a material reference, or -1."""
        hit = self.mr.texture(pkg, path, ref) if ref else None
        if not hit:
            return -1
        if hit in self.ids:
            return self.ids[hit]
        p = self.lib.package(hit[0])
        try:
            t = Texture(p, p.exports[hit[1] - 1])
            w, h, px = t.rgba(t.pick_mip(self.MAX_SIDE), self.palette)
        except Exception:
            self.ids[hit] = -1
            return -1
        alpha = any(px[i] < 250 for i in range(3, len(px), 4))
        png = png_bytes(w, h, px)
        self.items.append(dict(uri='data:image/png;base64,' + base64.b64encode(png).decode(),
                               alpha=alpha))
        self.ids[hit] = len(self.items) - 1
        self.sizes[self.ids[hit]] = t.mips[0][:2]
        return self.ids[hit]


def object_array(pkg, entry):
    """Object references held in an array property: a compact count, then
    that many compact indices."""
    if not entry or entry.get('type') != 9:
        return []
    r = R(pkg.b, entry['at'])
    return [r.idx() for _ in range(r.idx())]


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


def tint(name):
    """A stable pastel colour per material name, for the untextured BSP."""
    h = (zlib.crc32(name.encode()) % 360) / 360.0
    return colorsys.hsv_to_rgb(h, 0.35, 1.0)


# BSP surface flag 0x80, PF_FakeBackdrop: a window onto the sky. It is set on
# surfaces of the 17 levels with a SkyZoneInfo and of none of the 12 without,
# always facing a playable zone and backing onto the outside.
PF_FAKE_BACKDROP = 0x80
LEVEL, BACKDROP, SKY = 0, 1, 2


def build_bsp(pkg, path=None, textures=None, model=None, sky_zone=None):
    """BSP polygons, each triangle wound so it faces the way its node's plane
    does. Drawn front side only, a level can then be looked into from outside,
    since its faces point into the playable space.

    Texture coordinates come from the surface: a texel position is the offset
    from the surface's base point projected on its two texture vectors, and
    dividing by the texture's size makes it the 0 to 1 range of the image.
    Triangles are grouped by part and texture, as (first index, count, texture
    id, part), the part being the level, its sky windows, or the sky zone."""
    m = model or level_model(pkg)
    pos, col, uv, by_tex = [], [], [], collections.defaultdict(list)
    mats = collections.Counter()
    V, Pts = m.vectors, m.points
    for pts, s, flags, node in polygons(m, with_surf=True):
        nrm = node.plane[:3]
        part = (SKY if sky_zone is not None and node.zone == sky_zone
                else BACKDROP if flags & PF_FAKE_BACKDROP else LEVEL)
        mat = pkg.refname(s.material) if s else 'None'
        tid = textures.for_material(pkg, path, s.material) if (textures and s) else -1
        base = len(pos) // 3
        c = tint(mat)
        P = [conv(p) for p in pts]
        N = (nrm[0], nrm[2], nrm[1])
        if tid >= 0:
            us, vs = textures.sizes[tid]
            o, tu, tv = Pts[s.base], V[s.texture_u], V[s.texture_v]
        for p, q in zip(P, pts):
            pos += p
            col += [int(c[0] * 255), int(c[1] * 255), int(c[2] * 255)]
            if tid >= 0:
                d = (q[0] - o[0], q[1] - o[1], q[2] - o[2])
                uv += [(d[0] * tu[0] + d[1] * tu[1] + d[2] * tu[2]) / us,
                       (d[0] * tv[0] + d[1] * tv[1] + d[2] * tv[2]) / vs]
            else:
                uv += [0.0, 0.0]
        for i in range(1, len(P) - 1):
            g = cross(sub(P[i], P[0]), sub(P[i + 1], P[0]))
            if g[0] * N[0] + g[1] * N[1] + g[2] * N[2] >= 0:
                by_tex[part, tid] += [base, base + i, base + i + 1]
            else:
                by_tex[part, tid] += [base, base + i + 1, base + i]
        mats[mat] += 1
    idx, groups = [], []
    for part, tid in sorted(by_tex):
        groups.append([len(idx), len(by_tex[part, tid]), tid, part])
        idx += by_tex[part, tid]
    return pos, col, idx, mats, uv, groups


def build_terrain(pkg, files, textures=None, path=None):
    """Terrain grids, wound so each triangle faces up, with their texture
    layers: per layer a texture, the u and v rows of its matrix, and a weight
    per vertex. Coloured by height as well, for terrains without layers."""
    out = []
    for t in terrains(pkg, files):
        V = [conv(v) for v in t.vertices()]
        lo = min(v[1] for v in V)
        hi = max(v[1] for v in V)
        span = (hi - lo) or 1.0
        pos, col, idx = [], [], []
        for v in V:
            pos += v
            k = (v[1] - lo) / span                # low: dark mud, high: grass
            col += [int(70 + 60 * k), int(80 + 110 * k), int(45 + 35 * k)]
        for a, b, c in t.triangles():
            g = cross(sub(V[b], V[a]), sub(V[c], V[a]))
            idx += [a, b, c] if g[1] >= 0 else [a, c, b]
        layers, weights = [], []
        for lay in t.layers if textures else ():
            tid = textures.for_material(pkg, path, lay['texture'])
            if tid < 0:
                continue
            w = [255] * (t.X * t.Y)
            hit = textures.mr.texture(pkg, path, lay['alpha']) if lay['alpha'] else None
            if hit:
                ap = textures.lib.package(hit[0])
                aw, ah, apx = Texture(ap, ap.exports[hit[1] - 1]).rgba(0)
                w = t.weights(apx, aw, ah)
            layers.append(dict(tex=tid, u=lay['u'], v=lay['v']))
            weights += w
        out.append(dict(pos=b64('f', pos), col=b64('B', col), idx=b64('I', idx),
                        layers=layers, weights=b64('B', weights), tris=len(idx) // 3))
    return out


def instance_matrix(cols, loc):
    """three.js column major matrix for an actor transform: P A P, P the Y/Z swap."""
    p = (0, 2, 1)
    A3 = [[cols[p[c]][p[r]] for c in range(3)] for r in range(3)]
    t = conv(loc)
    return [A3[0][0], A3[1][0], A3[2][0], 0.0, A3[0][1], A3[1][1], A3[2][1], 0.0,
            A3[0][2], A3[1][2], A3[2][2], 0.0, t[0], t[1], t[2], 1.0]


def build_skeletal(pkg, path, lib, defaults, textures=None, in_sky=lambda loc: False):
    """Skeletal meshes in their reference pose, laid out per wedge so that each
    corner carries its own texture coordinates, with faces grouped by material."""
    uniq, inst = {}, collections.defaultdict(list)
    if defaults is None:
        return uniq, inst
    for key, sk, cols, loc, name in skeletal_instances(pkg, path, lib, defaults):
        if key not in uniq:
            pts, wedges, faces = skeletal_local_points(sk)
            pos, uv = [], []
            for w in wedges:
                pos += conv(pts[w[0]])
                uv += [w[1], w[2]]
            idx, groups = [], []
            for mat in sorted({f[3] for f in faces}):
                start = len(idx)
                for f in faces:
                    if f[3] == mat:
                        idx += [f[0], f[1], f[2]]
                ref = sk.textures[mat] if mat < len(sk.textures) else 0
                tid = textures.for_material(sk.p, key[0], ref) if textures else -1
                groups.append([start, len(idx) - start, tid])
            uniq[key] = (sk.name, pos, idx, uv, groups)
        inst[key, in_sky(loc)].append(instance_matrix(cols, loc))
    return uniq, inst


def build_meshes(pkg, path, lib, defaults=None, textures=None, in_sky=lambda loc: False):
    """Unique (mesh, skins) pairs once each, plus one transform per actor,
    kept apart by whether the actor stands in the sky zone."""
    uniq, inst = {}, collections.defaultdict(list)
    skins_of = {}
    for e, d in Map(pkg).actors(values=True, live=True):
        skins_of[e['name']] = object_array(pkg, d.get('Skins'))
    for key, mesh, cols, loc, name in static_mesh_instances(pkg, path, lib, defaults):
        skins = tuple(skins_of.get(name, ()))
        ukey = (key, skins)
        if ukey not in uniq:
            pos, uv = [], []
            for v in mesh.verts:
                pos += conv(v[:3])
            uvs = mesh.uvs[0] if mesh.uvs else [(0.0, 0.0)] * len(mesh.verts)
            for u in uvs:
                uv += [u[0], u[1]]
            groups = []
            mats = mesh_materials(mesh) or []
            for k, (first, nfaces) in enumerate(mesh.section_ranges()):
                if nfaces <= 0:
                    continue
                ref = skins[k] if k < len(skins) and skins[k] else (mats[k] if k < len(mats) else 0)
                hp, hpath = (pkg, path) if (k < len(skins) and skins[k]) else (mesh.p, key[0])
                tid = textures.for_material(hp, hpath, ref) if textures else -1
                groups.append([first, nfaces * 3, tid])
            uniq[ukey] = (mesh.name, pos, list(mesh.indices), uv, groups)
        inst[ukey, in_sky(loc)].append(instance_matrix(cols, loc))
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
<div id="info"><b>%(title)s</b><br>%(stats)s<br><br>
<b>Fly:</b> W A S D move · E or Space up · Q or C down · arrows look<br>
Shift fast · [ and ] change speed · R back to the overview<br>
mouse or touchpad drag also looks, wheel changes speed<br>
<b>Show:</b> B BSP one or both sides · M static meshes · T terrain · K skeletal meshes (off at first)<br>
Y sky as a background, or where it stands<br><br>%(legend)s</div>
<script src="%(three)s"></script>
<script>
function dec(s, T){const b=atob(s),u=new Uint8Array(b.length);for(let i=0;i<b.length;i++)u[i]=b.charCodeAt(i);return new T(u.buffer)}
const pos=dec("%(pos)s",Float32Array), col=dec("%(col)s",Uint8Array), idx=dec("%(idx)s",Uint32Array);
const g=new THREE.BufferGeometry();
g.setAttribute('position',new THREE.BufferAttribute(pos,3));
g.setAttribute('color',new THREE.BufferAttribute(col,3,true));
g.setAttribute('uv',new THREE.BufferAttribute(dec("%(uv)s",Float32Array),2));
g.setIndex(new THREE.BufferAttribute(idx,1));
g.computeVertexNormals(); g.computeBoundingSphere();
const scene=new THREE.Scene();
// Unreal texture coordinates start at the top left of the image, so textures
// are uploaded without the vertical flip three.js applies by default.
const TEX=%(textures)s.map(t=>{
  const tx=new THREE.TextureLoader().load(t.uri); tx.flipY=false;
  tx.wrapS=tx.wrapT=THREE.RepeatWrapping; return {tx, alpha:t.alpha};});
const matCache={};
function material(tid, fallback){
  const k=tid+'/'+fallback; if(matCache[k]) return matCache[k];
  const m = tid>=0
    ? new THREE.MeshLambertMaterial({map:TEX[tid].tx, side:THREE.DoubleSide, alphaTest:TEX[tid].alpha?0.5:0})
    : new THREE.MeshLambertMaterial({color:fallback, side:THREE.DoubleSide});
  return matCache[k]=m;}
// One instanced mesh per unique model, with a geometry group per material.
function instanced(list, group, fallback){
  for (const m of list){
    const mg=new THREE.BufferGeometry();
    mg.setAttribute('position',new THREE.BufferAttribute(dec(m.pos,Float32Array),3));
    mg.setAttribute('uv',new THREE.BufferAttribute(dec(m.uv,Float32Array),2));
    mg.setIndex(new THREE.BufferAttribute(dec(m.idx,Uint32Array),1));
    let mats=[material(-1, fallback)];
    if(m.groups.length){ mats=m.groups.map(gr=>material(gr[2], fallback));
      m.groups.forEach((gr,i)=>mg.addGroup(gr[0],gr[1],i)); }
    mg.computeVertexNormals();
    const mat=dec(m.mat,Float32Array), n=mat.length/16;
    const im=new THREE.InstancedMesh(mg, m.groups.length?mats:mats[0], n);
    const M=new THREE.Matrix4();
    for(let i=0;i<n;i++){M.fromArray(mat,i*16); im.setMatrixAt(i,M);}
    im.instanceMatrix.needsUpdate=true; group.add(im);
  }}
// BSP in three parts sharing one vertex buffer: the level, its windows onto
// the sky (PF_FakeBackdrop), and the sky zone. One material per texture, front
// side only; untextured surfaces keep their tint by material name.
const bspMats=[], parts=[[],[],[]];
%(groups)s.forEach(gr=>parts[gr[3]].push(gr));
function bspMesh(list){
  const bg=new THREE.BufferGeometry();
  for(const k of ['position','color','uv','normal']) bg.setAttribute(k,g.attributes[k]);
  const s=list.length?list[0][0]:0, e=list.length?list[list.length-1][0]+list[list.length-1][1]:0;
  bg.setIndex(new THREE.BufferAttribute(idx.subarray(s,e),1));
  const ms=list.map((gr,i)=>{ bg.addGroup(gr[0]-s,gr[1],i); const t=gr[2];
    const m = t>=0 ? new THREE.MeshLambertMaterial({map:TEX[t].tx, side:THREE.FrontSide, alphaTest:TEX[t].alpha?0.5:0})
                   : new THREE.MeshLambertMaterial({vertexColors:true, side:THREE.FrontSide});
    bspMats.push(m); return m; });
  return new THREE.Mesh(bg, ms.length?ms:new THREE.MeshBasicMaterial());
}
const levelBsp=bspMesh(parts[0]), backdrop=bspMesh(parts[1]); scene.add(levelBsp, backdrop);
const meshes=new THREE.Group(); scene.add(meshes);
const terrain=new THREE.Group(); scene.add(terrain);
// Off until K: they are for looking at, not needed to read the level.
const skeletal=new THREE.Group(); skeletal.visible=false; scene.add(skeletal);
// The sky zone, drawn first from SKY with the player camera's rotation, so it
// stays at infinity. Y shows it where it really stands instead.
const SKY=%(sky)s, skyGroup=new THREE.Group(), skyScene=new THREE.Scene();
skyGroup.add(bspMesh(parts[2]));
const skyMeshes=new THREE.Group(); skyGroup.add(skyMeshes);
const SKEL=%(skeletal)s, MESH=%(meshes)s;
instanced(SKEL.filter(m=>!m.sky), skeletal, 0xb08ce0);
instanced(SKEL.filter(m=>m.sky), skyMeshes, 0xb08ce0);
instanced(MESH.filter(m=>m.sky), skyMeshes, 0xc89070);
let skyAsBackground=!!SKY;
function placeSky(){
  (skyAsBackground?skyScene:scene).add(skyGroup);
  backdrop.visible=!skyAsBackground;
}
placeSky();
for(const sc of [scene, skyScene]){
  sc.add(new THREE.HemisphereLight(0xffffff,0x404050,0.75));
  const sun=new THREE.DirectionalLight(0xffffff,0.6); sun.position.set(0.4,1,0.3); sc.add(sun);
}
// Terrain layers are blended in order, each over what is below it by its own
// per vertex weight. Texture coordinates come from each layer's matrix applied
// to the Unreal position, which is the three.js position with Y and Z swapped
// back and scaled to Unreal units. Lit like the Lambert materials around it.
function terrainMaterial(layers){
  const n=layers.length, U=[], uni={};
  let vs='attribute vec4 w0; attribute vec4 w1; varying vec4 vw0; varying vec4 vw1; varying vec3 vn;\\n';
  let fs='varying vec4 vw0; varying vec4 vw1; varying vec3 vn;\\n';
  for(let i=0;i<n;i++){ vs+='uniform vec4 U'+i+'; uniform vec4 V'+i+'; varying vec2 t'+i+';\\n';
    fs+='uniform sampler2D T'+i+'; varying vec2 t'+i+';\\n';
    uni['U'+i]={value:new THREE.Vector4(...layers[i].u)}; uni['V'+i]={value:new THREE.Vector4(...layers[i].v)};
    uni['T'+i]={value:TEX[layers[i].tex].tx}; }
  vs+='void main(){ vec4 p=vec4(position.x*100.0, position.z*100.0, position.y*100.0, 1.0);\\n';
  for(let i=0;i<n;i++) vs+=' t'+i+'=vec2(dot(p,U'+i+'), dot(p,V'+i+'));\\n';
  vs+=' vw0=w0; vw1=w1; vn=normal; gl_Position=projectionMatrix*modelViewMatrix*vec4(position,1.0); }';
  const W=['vw0.x','vw0.y','vw0.z','vw0.w','vw1.x','vw1.y','vw1.z','vw1.w'];
  fs+='void main(){ vec3 c=vec3(0.0);\\n';
  for(let i=0;i<n;i++) fs+=' c=mix(c, texture2D(T'+i+', t'+i+').rgb, '+W[i]+');\\n';
  fs+=' vec3 nn=normalize(vn); float hemi=0.5*nn.y+0.5;\\n'+
      ' vec3 light=mix(vec3(0.25,0.25,0.31), vec3(1.0), hemi)*0.75 + max(dot(nn, normalize(vec3(0.4,1.0,0.3))),0.0)*0.6;\\n'+
      ' gl_FragColor=vec4(c*light,1.0); }';
  return new THREE.ShaderMaterial({uniforms:uni, vertexShader:vs, fragmentShader:fs});
}
for (const t of %(terrain)s){
  const tg=new THREE.BufferGeometry();
  tg.setAttribute('position',new THREE.BufferAttribute(dec(t.pos,Float32Array),3));
  tg.setAttribute('color',new THREE.BufferAttribute(dec(t.col,Uint8Array),3,true));
  tg.setIndex(new THREE.BufferAttribute(dec(t.idx,Uint32Array),1));
  tg.computeVertexNormals();
  const L=t.layers.slice(0,8), nv=tg.attributes.position.count, w=dec(t.weights,Uint8Array);
  let mat=new THREE.MeshLambertMaterial({vertexColors:true,side:THREE.FrontSide});
  if(L.length){
    const w0=new Float32Array(nv*4), w1=new Float32Array(nv*4);
    for(let i=0;i<L.length;i++) for(let v=0;v<nv;v++) (i<4?w0:w1)[v*4+(i%%4)]=w[i*nv+v]/255;
    tg.setAttribute('w0',new THREE.BufferAttribute(w0,4)); tg.setAttribute('w1',new THREE.BufferAttribute(w1,4));
    mat=terrainMaterial(L);
  }
  terrain.add(new THREE.Mesh(tg,mat));
}
instanced(MESH.filter(m=>!m.sky), meshes, 0xc89070);

// Frame the level on its BSP and terrain only: three.js bounds an InstancedMesh
// by its base geometry at the origin, not by where the instances are placed,
// and the sky zone stands far from the playable space.
const box=new THREE.Box3(), pv=new THREE.Vector3();
for(const li of [levelBsp, backdrop]){ const ix=li.geometry.index.array;
  for(let i=0;i<ix.length;i++){ pv.fromArray(pos, ix[i]*3); box.expandByPoint(pv); } }
if(box.isEmpty()) box.expandByPoint(pv.set(0,0,0));
terrain.children.forEach(m=>{m.geometry.computeBoundingBox(); box.union(m.geometry.boundingBox)});
const sph=box.getBoundingSphere(new THREE.Sphere()); const R=sph.radius, C=sph.center;
const cam=new THREE.PerspectiveCamera(60,innerWidth/innerHeight,Math.max(0.05,R/4000),R*20);
const skyCam=new THREE.PerspectiveCamera(60,innerWidth/innerHeight,0.05,R*20);
if(SKY) skyCam.position.fromArray(SKY);
const ren=new THREE.WebGLRenderer({antialias:true}); ren.setPixelRatio(devicePixelRatio);
ren.setSize(innerWidth,innerHeight); document.body.appendChild(ren.domElement);
ren.autoClear=false;
// Frames are drawn only when something changes, so an idle page costs nothing.
let dirty=true; const redraw=()=>{dirty=true};
THREE.DefaultLoadingManager.onLoad=redraw;
addEventListener('resize',()=>{for(const c of [cam,skyCam]){c.aspect=innerWidth/innerHeight;c.updateProjectionMatrix()}
  ren.setSize(innerWidth,innerHeight); redraw()});

// Noclip fly camera. Keys are read by physical position (event.code), so W is
// W on any layout, Cyrillic included.
let yaw=0, pitch=0, speed=R*0.15;
function overview(){
  cam.position.set(C.x-R*0.8,C.y+R*0.9,C.z-R*0.8);
  const d=new THREE.Vector3().subVectors(C,cam.position).normalize();
  yaw=Math.atan2(d.x,d.z); pitch=Math.asin(d.y);
}
overview();
const held={};
const PASS=['ArrowUp','ArrowDown','ArrowLeft','ArrowRight','Space'];
addEventListener('keydown',e=>{
  held[e.code]=true;
  if(PASS.includes(e.code)) e.preventDefault();
  if(e.repeat) return;
  if(e.code==='KeyB'){for(const m of bspMats){m.side=m.side===THREE.FrontSide?THREE.DoubleSide:THREE.FrontSide;m.needsUpdate=true}}
  if(e.code==='KeyM'){meshes.visible=!meshes.visible}
  if(e.code==='KeyT'){terrain.visible=!terrain.visible}
  if(e.code==='KeyK'){skeletal.visible=!skeletal.visible}
  if(e.code==='KeyR'){overview()}
  if(e.code==='KeyY'&&SKY){skyAsBackground=!skyAsBackground; placeSky()}
  redraw();
  if(e.code==='BracketRight'){speed*=1.5}
  if(e.code==='BracketLeft'){speed/=1.5}
});
addEventListener('keyup',e=>{held[e.code]=false; redraw()});
addEventListener('blur',()=>{for(const k in held) held[k]=false});
let dragging=false, lx=0, ly=0;
ren.domElement.addEventListener('pointerdown',e=>{dragging=true;lx=e.clientX;ly=e.clientY});
addEventListener('pointerup',()=>{dragging=false});
addEventListener('pointermove',e=>{
  if(!dragging) return;
  yaw-=(e.clientX-lx)*0.005; pitch-=(e.clientY-ly)*0.005; lx=e.clientX; ly=e.clientY; redraw();
});
ren.domElement.addEventListener('wheel',e=>{e.preventDefault(); speed*=e.deltaY<0?1.15:1/1.15},{passive:false});
const clock=new THREE.Clock(), up=new THREE.Vector3(0,1,0);
(function loop(){
  requestAnimationFrame(loop);
  const dt=Math.min(clock.getDelta(),0.1), h=k=>held[k]?1:0;
  const moving=['KeyW','KeyA','KeyS','KeyD','KeyE','KeyQ','KeyC','Space','ArrowUp','ArrowDown','ArrowLeft','ArrowRight'].some(k=>held[k]);
  if(!moving&&!dirty) return;
  dirty=false;
  yaw  +=(h('ArrowLeft')-h('ArrowRight'))*1.8*dt;
  pitch+=(h('ArrowUp')-h('ArrowDown'))*1.4*dt;
  pitch=Math.max(-1.55,Math.min(1.55,pitch));
  const fwd=new THREE.Vector3(Math.cos(pitch)*Math.sin(yaw),Math.sin(pitch),Math.cos(pitch)*Math.cos(yaw));
  const right=new THREE.Vector3().crossVectors(fwd,up).normalize();
  const v=new THREE.Vector3()
    .addScaledVector(fwd,h('KeyW')-h('KeyS'))
    .addScaledVector(right,h('KeyD')-h('KeyA'))
    .addScaledVector(up,Math.max(h('KeyE'),h('Space'))-Math.max(h('KeyQ'),h('KeyC')));
  const fast=(held['ShiftLeft']||held['ShiftRight'])?4:1;
  if(v.lengthSq()>0) cam.position.addScaledVector(v.normalize(),speed*fast*dt);
  cam.lookAt(cam.position.clone().add(fwd));
  ren.clear();
  if(skyAsBackground){ skyCam.quaternion.copy(cam.quaternion); ren.render(skyScene,skyCam); ren.clearDepth(); }
  ren.render(scene,cam);
})();
</script></body></html>
"""


def main(argv):
    src, out = argv[0], argv[1]
    meshdir = argv[2] if len(argv) > 2 else os.path.join(
        os.path.dirname(os.path.dirname(os.path.abspath(src))), 'StaticMeshes')
    pkg = Package(src)
    root = os.path.dirname(meshdir)
    lib = MeshLibrary([meshdir] + [os.path.join(root, d) for d in
                                   ('Textures', 'Animations', 'System', 'Maps')])
    sysdir = os.path.join(root, 'System')
    defaults = ClassDefaults(sysdir) if os.path.isdir(sysdir) else None
    textures = TextureTable(lib)
    model = level_model(pkg)
    # The sky is the zone its SkyZoneInfo stands in. The engine draws it as a
    # background, from that point, with the player camera's rotation.
    sky = next((d['Location']['value'] for e, d in Map(pkg).actors(values=True, live=True)
                if pkg.classof(e) == 'SkyZoneInfo' and 'Location' in d), None)
    sky_zone = model.zone_at(sky) if sky else None
    in_sky = lambda loc: sky_zone is not None and model.zone_at(loc) == sky_zone
    pos, col, idx, mats, bsp_uv, bsp_groups = build_bsp(pkg, src, textures, model, sky_zone)
    uniq, inst = build_meshes(pkg, src, lib, defaults, textures, in_sky)
    terr = build_terrain(pkg, lib.files, textures, src)
    suniq, sinst = build_skeletal(pkg, src, lib, defaults, textures, in_sky)

    def instances_json(uniq, inst):
        return json.dumps([dict(pos=b64('f', uniq[k][1]), idx=b64('I', uniq[k][2]),
                                uv=b64('f', uniq[k][3]), groups=uniq[k][4], sky=s,
                                mat=b64('f', [x for m in inst[k, s] for x in m]))
                           for k, s in inst])
    skel_json = instances_json(suniq, sinst)
    mesh_json = instances_json(uniq, inst)
    ninst = sum(len(v) for v in inst.values())
    ntri = sum(len(uniq[k][2]) // 3 * len(v) for (k, s), v in inst.items())
    title = os.path.splitext(os.path.basename(src))[0]
    legend = '<br>'.join(
        '<span class="sw" style="background:rgb(%d,%d,%d)"></span>%s (%d)'
        % (tuple(int(v * 255) for v in tint(n)) + (n, k))
        for n, k in mats.most_common(12))
    html = PAGE % dict(title=title, three=THREE,
                       stats='BSP %d polygons, %d triangles<br>static meshes %d placed, %d unique, %d triangles<br>terrain %d grids, %d triangles<br>skeletal meshes %d placed, %d unique, reference pose'
                             % (sum(mats.values()), len(idx) // 3, ninst, len(uniq), ntri,
                                len(terr), sum(t['tris'] for t in terr),
                                sum(len(v) for v in sinst.values()), len(suniq)),
                       legend=legend, pos=b64('f', pos), col=b64('B', col), idx=b64('I', idx),
                       uv=b64('f', bsp_uv), groups=json.dumps(bsp_groups),
                       meshes=mesh_json,
                       terrain=json.dumps([{k: t[k] for k in ('pos', 'col', 'idx', 'layers', 'weights')} for t in terr]),
                       skeletal=skel_json,
                       sky=json.dumps(conv(sky) if sky else None),
                       textures=json.dumps(textures.items))
    os.makedirs(os.path.dirname(os.path.abspath(out)), exist_ok=True)
    open(out, 'w').write(html)
    print('%s: BSP %d triangles, %d mesh instances (%d unique), %.1f MB -> %s'
          % (title, len(idx) // 3, ninst, len(uniq), len(html) / 1e6, out))


if __name__ == '__main__':
    main(sys.argv[1:])
