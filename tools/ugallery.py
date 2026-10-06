"""A browser gallery of skeletal meshes with their animations playing.

One page per mesh, plus an index. Each page carries the mesh, its skeleton and
its default animation inline and skins it on the CPU in JavaScript, with the
same arithmetic as uanim.skin: SKIN_JS below is a line by line port. Keeping the
port that close is deliberate. Re-deriving the pose through three.js bones would
mean converting quaternions across the Y/Z swap, a second place for a silent
sign error; instead the whole mesh space to view transform is one fixed matrix
on the object, and the JavaScript can be checked against Python numerically.

The output contains the game's own meshes and animations: it goes to out/, which
is gitignored, and is for looking at your own copy.
"""
import sys, os, json, base64, struct, glob, html

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from upkg import Package
from uskel import SkeletalMesh
from uanim import MeshAnimation
from ulevel import rotation_axes, MeshLibrary
from uview import TextureTable

THREE = 'https://cdnjs.cloudflare.com/ajax/libs/three.js/r128/three.min.js'

# Port of uanim.qmul, qconj, qrot, qnlerp, sample, compose and skin.
SKIN_JS = r"""
function qmul(a,b){const ax=a[0],ay=a[1],az=a[2],aw=a[3],bx=b[0],by=b[1],bz=b[2],bw=b[3];
  return [aw*bx+ax*bw+ay*bz-az*by, aw*by-ax*bz+ay*bw+az*bx, aw*bz+ax*by-ay*bx+az*bw, aw*bw-ax*bx-ay*by-az*bz];}
function qconj(q){return [-q[0],-q[1],-q[2],q[3]];}
function qrot(q,v){return qmul(qmul(q,[v[0],v[1],v[2],0]),qconj(q)).slice(0,3);}
function qnlerp(a,b,t){
  if(a[0]*b[0]+a[1]*b[1]+a[2]*b[2]+a[3]*b[3]<0) b=[-b[0],-b[1],-b[2],-b[3]];
  const q=[0,1,2,3].map(i=>a[i]+(b[i]-a[i])*t), n=Math.hypot(q[0],q[1],q[2],q[3])||1;
  return q.map(x=>x/n);}
function vlerp(a,b,t){return [0,1,2].map(i=>a[i]+(b[i]-a[i])*t);}
function keyAt(keys,times,frame,lerp,ref){
  if(!keys.length) return ref;
  const T=times.length===keys.length?times:keys.map((_,i)=>i);
  if(frame<=T[0]||keys.length===1) return keys[0];
  for(let i=1;i<keys.length;i++) if(frame<=T[i]){
    const t0=T[i-1],t1=T[i], f=t1>t0?(frame-t0)/(t1-t0):0; return lerp(keys[i-1],keys[i],f);}
  return keys[keys.length-1];}
function compose(parents,locals){
  const G=[];
  for(let i=0;i<parents.length;i++){
    let lq=locals[i][0]; const lp=locals[i][1];
    if(i===0){G.push([lq,lp]);continue;}
    lq=qconj(lq); const P=G[parents[i]], r=qrot(P[0],lp);
    G.push([qmul(P[0],lq),[P[1][0]+r[0],P[1][1]+r[1],P[1][2]+r[2]]]);}
  return G;}
function skin(M,seq,frame,out){
  const locals=M.refLocal.map((rl,b)=>{
    const t=seq?seq.tracks[b]:null;
    if(!t) return rl;
    return [keyAt(t.q,t.t,frame,qnlerp,rl[0]), keyAt(t.p,t.t,frame,vlerp,rl[1])];});
  const cur=compose(M.parents,locals), ref=M.refGlobal, n=M.points.length/3;
  const acc=new Float64Array(n*4);
  for(let i=0;i<M.infl.length;i+=3){
    const w=M.infl[i], p=M.infl[i+1], b=M.infl[i+2];
    if(w<=0||p>=n||b>=cur.length) continue;
    const rq=ref[b][0], rp=ref[b][1], cq=cur[b][0], cp=cur[b][1];
    const local=qrot(qconj(rq),[M.points[3*p]-rp[0],M.points[3*p+1]-rp[1],M.points[3*p+2]-rp[2]]);
    const wd=qrot(cq,local);
    acc[4*p]+=w*(wd[0]+cp[0]); acc[4*p+1]+=w*(wd[1]+cp[1]); acc[4*p+2]+=w*(wd[2]+cp[2]); acc[4*p+3]+=w;}
  for(let p=0;p<n;p++){
    const s=acc[4*p+3];
    if(s>0){out[3*p]=acc[4*p]/s; out[3*p+1]=acc[4*p+1]/s; out[3*p+2]=acc[4*p+2]/s;}
    else{out[3*p]=M.points[3*p]; out[3*p+1]=M.points[3*p+1]; out[3*p+2]=M.points[3*p+2];}}
  return out;}
"""


def track_for(anim, seq_index, bone_name):
    names = [b[0] for b in anim.bones]
    if bone_name not in names:
        return None
    chunk = anim.chunks[seq_index]
    j = names.index(bone_name)
    if chunk.bone_indices:
        return chunk.tracks[chunk.bone_indices.index(j)] if j in chunk.bone_indices else None
    return chunk.tracks[j] if j < len(chunk.tracks) else None


def export(sk, anim, textures=None, path=None):
    """Everything a page needs, as plain lists, in mesh space. Positions are
    per point, since that is what skinning moves; the drawn geometry is per
    wedge, a point with its own texture coordinates, and its triangles are
    grouped by material."""
    import uanim
    bones, _ = sk.skeleton()
    pts, wedges, faces = sk.geometry(0)
    _, n, s, _, _ = sk.lods[0]['influences']
    b = sk.p.b
    infl = []
    for i in range(n):
        w, pi, bi = struct.unpack_from('<fHH', b, s + 8 * i)
        infl += [w, pi, bi]
    ref_local = [[list(bn[2]), list(bn[3])] for bn in bones]
    ref_global = uanim.compose(bones, [(bn[2], bn[3]) for bn in bones])
    seqs = []
    if anim:
        for si, seq in enumerate(anim.sequences):
            tracks = []
            for bn in bones:
                t = track_for(anim, si, bn[0])
                if t is None:
                    tracks.append(None)
                    continue
                q = [list(k) if any(abs(x) > 1e-6 for x in k) else list(bn[2]) for k in t.quats]
                tracks.append(dict(q=q, p=[list(k) for k in t.positions], t=list(t.times)))
            seqs.append(dict(name=seq.name, frames=seq.num_frames, rate=seq.rate or 30.0,
                             tracks=tracks))
    tris, groups = [], []
    for mat in sorted({f[3] for f in faces}):
        start = len(tris)
        tris += [f[k] for f in faces if f[3] == mat for k in range(3)]
        ref = sk.textures[mat] if mat < len(sk.textures) else 0
        tid = textures.for_material(sk.p, path, ref) if textures else -1
        groups.append([start, len(tris) - start, tid])
    return dict(points=[c for v in pts for c in v],
                wedgePoint=[w[0] for w in wedges], uv=[c for w in wedges for c in w[1:3]],
                tris=tris, groups=groups,
                infl=infl, parents=[bn[5] for bn in bones], refLocal=ref_local,
                refGlobal=[[list(g[0]), list(g[1])] for g in ref_global],
                seqs=seqs, bones=[bn[0] for bn in bones])


def view_matrix(sk):
    """Column major 4x4: mesh space to the viewer, in metres, Y up. The mesh
    goes into actor space as RotOrigin applied to (point - MeshOrigin) * MeshScale,
    then Unreal's Z up, left handed axes become Y up, right handed by one Y/Z swap."""
    X, Y, Z = rotation_axes(*sk.rot_origin)
    R = [[X[r], Y[r], Z[r]] for r in range(3)]
    L = [[R[r][c] * sk.scale[c] for c in range(3)] for r in range(3)]
    P = (0, 2, 1)
    L = [[L[P[r]][c] / 100.0 for c in range(3)] for r in range(3)]
    t = [-sum(L[r][c] * sk.origin[c] for c in range(3)) for r in range(3)]
    return [L[0][0], L[1][0], L[2][0], 0, L[0][1], L[1][1], L[2][1], 0,
            L[0][2], L[1][2], L[2][2], 0, t[0], t[1], t[2], 1]


PAGE = """<!doctype html><html><head><meta charset="utf-8"><title>%(title)s</title>
<style>
html,body{margin:0;height:100%%;background:#1b1b22;overflow:hidden;font:13px system-ui,sans-serif;color:#ddd}
#info{position:fixed;top:10px;left:12px;background:rgba(0,0,0,.6);padding:9px 12px;border-radius:6px;line-height:1.55;max-width:46em}
#info b{color:#fff} select{background:#2a2a33;color:#eee;border:1px solid #555;border-radius:4px;max-width:22em}
a{color:#9cf}
</style></head><body>
<div id="info"><b>%(title)s</b> <span style="opacity:.7">from %(package)s, %(bones)d bones, %(npts)d points</span><br>
<select id="seq"></select> <span id="state"></span><br>
<b>Play:</b> Space play or pause · , and . step a frame · [ and ] previous or next sequence · - and = speed<br>
<b>View:</b> arrows orbit · W and S zoom · R reset<br>
<b>Pages:</b> PageUp and PageDown previous or next mesh · Home the index · <a href="index.html">index</a></div>
<script src="%(three)s"></script>
<script>
%(skin)s
const M=%(data)s, PREV=%(prev)s, NEXT=%(next)s;
const scene=new THREE.Scene();
scene.add(new THREE.HemisphereLight(0xffffff,0x404050,0.8));
const sun=new THREE.DirectionalLight(0xffffff,0.6); sun.position.set(0.5,1,0.7); scene.add(sun);
const grid=new THREE.GridHelper(4,16,0x555566,0x33333d); scene.add(grid);
const n=M.points.length/3, pos=new Float32Array(M.points);
// Drawn per wedge: each corner has its own texture coordinates. Normals are
// summed per point, so the seams between UV islands do not show as creases.
const nw=M.wedgePoint.length, wpos=new Float32Array(nw*3), wnor=new Float32Array(nw*3), pnor=new Float32Array(n*3);
function toWedges(){
  pnor.fill(0); const t=M.tris, W=M.wedgePoint;
  for(let i=0;i<t.length;i+=3){
    const a=W[t[i]]*3, b=W[t[i+1]]*3, c=W[t[i+2]]*3;
    const ux=pos[b]-pos[a], uy=pos[b+1]-pos[a+1], uz=pos[b+2]-pos[a+2];
    const vx=pos[c]-pos[a], vy=pos[c+1]-pos[a+1], vz=pos[c+2]-pos[a+2];
    const nx=uy*vz-uz*vy, ny=uz*vx-ux*vz, nz=ux*vy-uy*vx;
    for(const k of [a,b,c]){pnor[k]+=nx; pnor[k+1]+=ny; pnor[k+2]+=nz;}
  }
  for(let w=0;w<nw;w++){const p=W[w]*3;
    for(let j=0;j<3;j++){wpos[w*3+j]=pos[p+j]; wnor[w*3+j]=pnor[p+j];}}
}
toWedges();
const g=new THREE.BufferGeometry();
g.setAttribute('position',new THREE.BufferAttribute(wpos,3));
g.setAttribute('normal',new THREE.BufferAttribute(wnor,3));
g.setAttribute('uv',new THREE.BufferAttribute(new Float32Array(M.uv),2));
g.setIndex(new THREE.BufferAttribute(new Uint32Array(M.tris),1));
// Unreal texture coordinates start at the top left, so no vertical flip.
const TEX=%(textures)s.map(t=>{const tx=new THREE.TextureLoader().load(t.uri);
  tx.flipY=false; tx.wrapS=tx.wrapT=THREE.RepeatWrapping; return {tx,alpha:t.alpha};});
const mats=M.groups.map((gr,i)=>{g.addGroup(gr[0],gr[1],i); const t=gr[2];
  return t>=0 ? new THREE.MeshLambertMaterial({map:TEX[t].tx,side:THREE.DoubleSide,alphaTest:TEX[t].alpha?0.5:0})
              : new THREE.MeshLambertMaterial({color:0x9ccf7a,side:THREE.DoubleSide});});
const mesh=new THREE.Mesh(g,mats);
mesh.matrixAutoUpdate=false; mesh.matrix.fromArray(%(matrix)s); scene.add(mesh);
g.computeBoundingBox();
const bb=g.boundingBox.clone().applyMatrix4(mesh.matrix), C=bb.getCenter(new THREE.Vector3()), R=Math.max(0.3,bb.getSize(new THREE.Vector3()).length());
grid.position.y=bb.min.y;
const cam=new THREE.PerspectiveCamera(45,innerWidth/innerHeight,0.01,100);
const ren=new THREE.WebGLRenderer({antialias:true}); ren.setPixelRatio(devicePixelRatio);
ren.setSize(innerWidth,innerHeight); document.body.appendChild(ren.domElement);
addEventListener('resize',()=>{cam.aspect=innerWidth/innerHeight;cam.updateProjectionMatrix();ren.setSize(innerWidth,innerHeight)});
let yaw=0.6, pitch=0.15, dist=R*1.6;
function reset(){yaw=0.6;pitch=0.15;dist=R*1.6}
const sel=document.getElementById('seq'), state=document.getElementById('state');
M.seqs.forEach((s,i)=>{const o=document.createElement('option');o.value=i;o.textContent=s.name+' ('+s.frames+' frames)';sel.appendChild(o)});
let si=Math.max(0,M.seqs.findIndex(s=>/^idle/i.test(s.name))), frame=0, playing=true, speed=1;
if(!M.seqs.length){sel.style.display='none'}
sel.value=si; sel.addEventListener('change',()=>{si=+sel.value;frame=0;sel.blur()});
const held={};
addEventListener('keydown',e=>{
  held[e.code]=true;
  if(['ArrowUp','ArrowDown','ArrowLeft','ArrowRight','Space'].includes(e.code)) e.preventDefault();
  if(e.repeat&&!['Comma','Period'].includes(e.code)) return;
  const S=M.seqs.length;
  if(e.code==='Space') playing=!playing;
  if(e.code==='Comma'&&S){playing=false;frame=Math.max(0,Math.floor(frame)-1)}
  if(e.code==='Period'&&S){playing=false;frame=(Math.floor(frame)+1)%%Math.max(1,M.seqs[si].frames)}
  if(e.code==='BracketLeft'&&S){si=(si-1+S)%%S;frame=0;sel.value=si}
  if(e.code==='BracketRight'&&S){si=(si+1)%%S;frame=0;sel.value=si}
  if(e.code==='Minus') speed=Math.max(0.1,speed/1.5);
  if(e.code==='Equal') speed=Math.min(8,speed*1.5);
  if(e.code==='KeyR') reset();
  if(e.code==='PageUp'&&PREV) location.href=PREV;
  if(e.code==='PageDown'&&NEXT) location.href=NEXT;
  if(e.code==='Home') location.href='index.html';
  dirty=true;
});
addEventListener('keyup',e=>{held[e.code]=false; dirty=true});
addEventListener('blur',()=>{for(const k in held) held[k]=false});
const clock=new THREE.Clock(), out=new Float64Array(n*3);
// A frame is drawn only while the animation plays, a view key is held, or
// something changed, so a paused or still page costs nothing.
let dirty=true;
THREE.DefaultLoadingManager.onLoad=()=>{dirty=true};
addEventListener('resize',()=>{dirty=true});
sel.addEventListener('change',()=>{dirty=true});
(function loop(){
  requestAnimationFrame(loop);
  const dt=Math.min(clock.getDelta(),0.1), h=k=>held[k]?1:0;
  const viewing=['ArrowUp','ArrowDown','ArrowLeft','ArrowRight','KeyW','KeyS'].some(k=>held[k]);
  if(!(playing&&M.seqs.length)&&!viewing&&!dirty) return;
  dirty=false;
  yaw+=(h('ArrowLeft')-h('ArrowRight'))*1.6*dt; pitch+=(h('ArrowUp')-h('ArrowDown'))*1.2*dt;
  pitch=Math.max(-1.4,Math.min(1.4,pitch)); dist*=Math.pow(1.8,(h('KeyS')-h('KeyW'))*dt);
  cam.position.set(C.x+dist*Math.cos(pitch)*Math.sin(yaw),C.y+dist*Math.sin(pitch),C.z+dist*Math.cos(pitch)*Math.cos(yaw));
  cam.lookAt(C);
  const s=M.seqs[si];
  if(s){
    if(playing) frame=(frame+dt*s.rate*speed)%%Math.max(1,s.frames);
    skin(M,s,frame,out); for(let i=0;i<out.length;i++) pos[i]=out[i];
    toWedges(); g.attributes.position.needsUpdate=true; g.attributes.normal.needsUpdate=true;
    state.textContent='frame '+frame.toFixed(1)+' of '+s.frames+' at '+s.rate+' fps'+(speed!==1?' x'+speed.toFixed(2):'')+(playing?'':' paused');
  } else state.textContent='no animation, reference pose';
  ren.render(scene,cam);
})();
</script></body></html>
"""

INDEX = """<!doctype html><html><head><meta charset="utf-8"><title>Skeletal mesh gallery</title>
<style>body{margin:0;padding:24px 32px;background:#1b1b22;color:#ddd;font:14px system-ui,sans-serif}
h1{font-size:20px;color:#fff} h2{font-size:15px;color:#bbb;margin-top:22px}
a{color:#9cf;text-decoration:none} a:hover{text-decoration:underline}
ul{columns:4 14em;padding-left:18px;line-height:1.7} span{opacity:.55}</style></head><body>
<h1>Skeletal mesh gallery</h1>
<p>%(count)d meshes. Open one and use PageUp and PageDown to move between them.</p>
%(body)s
</body></html>
"""


def rounded(x, digits=6):
    """Floats to `digits` significant figures, recursively. Seventeen digits per
    number is what bloats a page; six keep the pose to well under a millimetre."""
    if isinstance(x, float):
        return float('%.*g' % (digits, x))
    if isinstance(x, list):
        return [rounded(v, digits) for v in x]
    if isinstance(x, dict):
        return {k: rounded(v, digits) for k, v in x.items()}
    return x


def safe(name):
    return ''.join(c if c.isalnum() or c in '-_' else '_' for c in name)


def main(argv):
    src, out = argv[0], argv[1]
    os.makedirs(out, exist_ok=True)
    root = os.path.dirname(os.path.abspath(src))
    lib = MeshLibrary([os.path.join(root, d) for d in
                       ('Animations', 'Textures', 'StaticMeshes', 'System', 'Maps')])
    entries = []
    for f in sorted(glob.glob(os.path.join(src, '*.ukx'))):
        p = Package(f)
        pkg = os.path.basename(f)
        for e in p.exports:
            if p.classof(e) == 'SkeletalMesh' and e['size']:
                entries.append((pkg, p, e))
    pages = ['%s_%s.html' % (safe(os.path.splitext(pkg)[0]), safe(e['name'])) for pkg, p, e in entries]
    groups, written = {}, 0
    for i, (pkg, p, e) in enumerate(entries):
        sk = SkeletalMesh(p, e)
        bones, aref = sk.skeleton()
        if not bones or not sk.lods:
            continue
        anim = None
        if aref > 0 and p.classof(p.exports[aref - 1]) == 'MeshAnimation':
            anim = MeshAnimation(p, p.exports[aref - 1])
        textures = TextureTable(lib)
        data = rounded(export(sk, anim, textures, os.path.join(src, pkg)))
        page = PAGE % dict(title=html.escape(e['name']), package=html.escape(pkg),
                           bones=len(bones), npts=len(data['points']) // 3,
                           three=THREE, skin=SKIN_JS,
                           textures=json.dumps(textures.items),
                           data=json.dumps(data, separators=(',', ':')),
                           matrix=json.dumps(view_matrix(sk)),
                           prev=json.dumps(pages[i - 1] if i > 0 else None),
                           next=json.dumps(pages[i + 1] if i + 1 < len(pages) else None))
        open(os.path.join(out, pages[i]), 'w').write(page)
        groups.setdefault(pkg, []).append('<li><a href="%s">%s</a> <span>%d sequences</span></li>'
                                          % (pages[i], html.escape(e['name']), len(data['seqs'])))
        written += 1
    body = ''.join('<h2>%s</h2><ul>%s</ul>' % (html.escape(k), ''.join(v)) for k, v in groups.items())
    open(os.path.join(out, 'index.html'), 'w').write(INDEX % dict(count=written, body=body))
    print('%d mesh pages and an index written to %s' % (written, out))


if __name__ == '__main__':
    main(sys.argv[1:])
