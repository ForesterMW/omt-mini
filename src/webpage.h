// The control panel, served from memory. One file, no dependencies, because it
// has to work on a machine that has never seen this page before.
#pragma once

inline const char* kControlPanelHtml = R"HTMLPAGE(<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>OMT Mini control panel (beta)</title>
<style>
:root{
  --bg:#14161a; --panel:#1c1f25; --panel-hi:#262a32; --panel-sel:#2e333d;
  --border:#303540; --text:#e8eaed; --dim:#9aa2af; --accent:#3b82f6;
  --accent-hi:#60a5fa; --ok:#22c55e; --danger:#ef4444; --warn:#f59e0b;
  --violet:#a78bfa; --radius:8px;
}
*{box-sizing:border-box}
html,body{height:100%}
body{
  margin:0;background:var(--bg);color:var(--text);
  font:14px/1.45 "Segoe UI",system-ui,-apple-system,sans-serif;
  overflow:hidden;
}
header{
  height:52px;display:flex;align-items:center;gap:14px;padding:0 18px;
  background:var(--panel);border-bottom:1px solid var(--border);
}
header h1{font-size:15px;font-weight:600;margin:0;letter-spacing:.2px}
header .host{color:var(--dim);font-size:12px}
.spacer{flex:1}
.dot{width:8px;height:8px;border-radius:50%;background:var(--danger);flex:none}
.dot.live{background:var(--ok)}
button{
  font:inherit;color:var(--text);background:var(--panel-hi);
  border:1px solid var(--border);border-radius:6px;padding:7px 13px;cursor:pointer;
}
button:hover{background:var(--panel-sel)}
button.primary{background:var(--accent);border-color:var(--accent);color:#fff}
button.primary:hover{background:var(--accent-hi)}
button.ghost{background:transparent;border-color:transparent;color:var(--dim)}
button.ghost:hover{color:var(--text);background:var(--panel-hi)}
main{display:grid;grid-template-columns:288px 1fr;height:calc(100% - 52px)}
aside{
  border-right:1px solid var(--border);background:var(--panel);
  display:flex;flex-direction:column;min-height:0;
}
aside h2,section h2{
  font-size:11px;text-transform:uppercase;letter-spacing:.9px;color:var(--dim);
  margin:0;padding:16px 16px 8px;font-weight:600;
}
#search{
  margin:0 16px 10px;padding:8px 10px;border-radius:6px;background:var(--bg);
  border:1px solid var(--border);color:var(--text);font:inherit;
}
#search:focus{outline:none;border-color:var(--accent)}
#sources{overflow-y:auto;padding:0 12px 12px;flex:1;min-height:0}
.src{
  background:var(--bg);border:1px solid var(--border);border-radius:var(--radius);
  padding:9px 11px;margin-bottom:7px;cursor:grab;user-select:none;
}
.src:hover{background:var(--panel-hi);border-color:var(--accent)}
.src:active{cursor:grabbing}
.src .name{font-weight:600;display:flex;align-items:center;gap:7px}
.src .addr{color:var(--dim);font-size:11.5px;margin-top:2px;
  overflow:hidden;text-overflow:ellipsis;white-space:nowrap}
.tag{
  font-size:10px;padding:1px 7px;border-radius:9px;border:1px solid;
  font-weight:600;letter-spacing:.2px;flex:none;
}
.tag.direct{color:var(--violet);border-color:var(--violet);background:#a78bfa22}
.tag.local{color:var(--accent-hi);border-color:var(--accent-hi);background:#60a5fa22}
.tag.mini{color:var(--ok);border-color:var(--ok);background:#22c55e22}
.tag.mdns{color:var(--ok);border-color:var(--ok);background:#22c55e22}
.tag.beta{color:var(--warn);border-color:var(--warn);background:#f59e0b22;
  text-transform:uppercase;letter-spacing:.6px}
.tag.off{color:var(--danger);border-color:var(--danger);background:#ef444422}
.wrap{overflow-y:auto;padding:0 20px 24px;min-height:0}
section{margin-top:4px}
.bar{display:flex;align-items:center;gap:10px;padding:16px 0 10px}
.bar h2{padding:0;flex:1}
#desk{
  position:relative;background:#0b0d10;border:1px solid var(--border);
  border-radius:var(--radius);overflow:hidden;
}
#desk .mon{position:absolute;border:1px dashed #ffffff18;border-radius:4px}
.win{
  position:absolute;background:var(--panel);border:1px solid var(--border);
  border-radius:6px;overflow:hidden;cursor:move;user-select:none;
  box-shadow:0 2px 10px #0006;
}
.win.mv{border-color:var(--violet)}
.win.drop{border-color:var(--accent);background:var(--panel-sel)}
.win.min{opacity:.5;border-style:dashed}
.win.min .cap{color:var(--dim)}
.win .cap{
  padding:5px 8px;font-size:11.5px;font-weight:600;
  overflow:hidden;text-overflow:ellipsis;white-space:nowrap;
  border-bottom:1px solid var(--border);display:flex;gap:6px;align-items:center;
}
.win .body{padding:6px 8px;font-size:11px;color:var(--dim);
  overflow:hidden;text-overflow:ellipsis;white-space:nowrap}
.win .x,.win .m{
  position:absolute;top:3px;width:18px;height:18px;line-height:16px;
  text-align:center;border-radius:4px;color:var(--dim);cursor:pointer;font-size:14px;
}
.win .x{right:4px}
.win .m{right:24px;font-size:15px}
.win .x:hover{background:var(--danger);color:#fff}
.win .m:hover{background:var(--panel-sel);color:var(--text)}
.win .grip{
  position:absolute;right:0;bottom:0;width:14px;height:14px;cursor:nwse-resize;
  background:linear-gradient(135deg,transparent 50%,var(--dim) 50%);
  border-bottom-right-radius:6px;
}
#tiles{display:grid;gap:6px;background:#0b0d10;border:1px solid var(--border);
  border-radius:var(--radius);padding:6px}
.tile{
  background:var(--panel);border:1px solid var(--border);border-radius:5px;
  min-height:58px;padding:7px 9px;font-size:11.5px;display:flex;flex-direction:column;
  justify-content:space-between;overflow:hidden;
}
.tile.drop{border-color:var(--accent);background:var(--panel-sel)}
.tile .t{font-weight:600;overflow:hidden;text-overflow:ellipsis;white-space:nowrap}
.tile .e{color:var(--dim)}
.tile .clr{color:var(--dim);cursor:pointer;font-size:11px;align-self:flex-start}
.tile .clr:hover{color:var(--danger)}
.empty{color:var(--dim);padding:22px;text-align:center;font-size:13px}
.row{display:flex;gap:8px;align-items:center}
input.addr{
  flex:1;padding:8px 10px;border-radius:6px;background:var(--bg);
  border:1px solid var(--border);color:var(--text);font:inherit;
}
input.addr:focus{outline:none;border-color:var(--accent)}
select{
  font:inherit;color:var(--text);background:var(--panel-hi);
  border:1px solid var(--border);border-radius:6px;padding:7px 9px;
}
#toast{
  position:fixed;left:50%;bottom:26px;transform:translateX(-50%);
  background:#0b0d10ee;border:1px solid var(--border);border-radius:16px;
  padding:9px 18px;font-size:12.5px;opacity:0;transition:opacity .18s;
  pointer-events:none;
}
#toast.on{opacity:1}
.hint{color:var(--dim);font-size:12px;padding:0 0 10px}
#machines{display:flex;flex-direction:column;gap:7px}
.mach{
  display:flex;align-items:center;gap:11px;background:var(--panel);
  border:1px solid var(--border);border-radius:var(--radius);padding:10px 13px;
}
.mach .n{font-weight:600}
.mach .v{color:var(--dim);font-size:12px;font-family:Consolas,monospace}
.mach .sp{flex:1}
.mach.behind{border-color:var(--warn)}
button.warn{background:var(--warn);border-color:var(--warn);color:#241a02;font-weight:600}
button.warn:hover{filter:brightness(1.1)}
button:disabled{opacity:.45;cursor:default}
</style>
</head>
<body>
<header>
  <div class="dot" id="live"></div>
  <h1>OMT Mini</h1>
  <span class="tag beta">beta</span>
  <span class="host" id="host"></span>
  <div class="spacer"></div>
  <button class="ghost" id="mvopen">Open multiview</button>
  <button class="primary" id="addbtn">Add source</button>
</header>

<main>
  <aside>
    <h2>Sources</h2>
    <input id="search" placeholder="Filter">
    <div id="sources"></div>
  </aside>

  <div class="wrap">
    <section>
      <div class="bar">
        <h2>Windows</h2>
        <span class="host" id="deskinfo"></span>
      </div>
      <div class="hint">
        Drag a source onto a window to change it. Drag a window to move it,
        the corner to resize, double click to maximise, and the buttons on a
        window to minimise or close it.
      </div>
      <div id="desk"></div>
    </section>

    <section id="fleetsec">
      <div class="bar">
        <h2>OMT Mini machines</h2>
        <span class="host" id="myver"></span>
      </div>
      <div class="hint">
        Other copies found on the network. Updating one closes its viewers and
        stops its capture and multiview output, so it asks first.
      </div>
      <div id="machines"></div>
    </section>

    <section id="mvsec">
      <div class="bar">
        <h2>Multiview</h2>
        <select id="layout"></select>
      </div>
      <div class="hint">Drag a source onto a cell.</div>
      <div id="tiles"></div>
    </section>
  </div>
</main>

<div id="toast"></div>

<script>
const LAYOUTS=[
 {n:"2 x 2",c:2,r:2,h:0},{n:"3 x 2",c:3,r:2,h:0},{n:"3 x 3",c:3,r:3,h:0},
 {n:"4 x 3",c:4,r:3,h:0},{n:"4 x 4",c:4,r:4,h:0},{n:"1 + 5",c:3,r:3,h:2},
 {n:"1 + 7",c:4,r:4,h:3}];

let state=null, sources=[], dragging=null, busy=false, filter="";
let lastSig="", fails=0, timer=null;
// Where the user just put a window, held until the machine agrees. Without
// this a drop shows the old position for one frame and visibly snaps back.
const optimistic=new Map();
// Windows told to close, hidden at once rather than lingering until the
// machine gets round to it.
const closing=new Set();
// A redraw between the two halves of a double click replaces the element and
// the second click lands on nothing, so interaction briefly holds redraws off.
let holdRenderUntil=0;

const $=id=>document.getElementById(id);
function toast(t){const e=$("toast");e.textContent=t;e.classList.add("on");
  clearTimeout(e.t);e.t=setTimeout(()=>e.classList.remove("on"),2200);}

// Every request is bounded. A stalled one must never wedge the page.
async function api(path,body){
  const ctl=new AbortController();
  const bail=setTimeout(()=>ctl.abort(),4000);
  try{
    const r=await fetch(path,{method:body?"POST":"GET",
      headers:body?{"Content-Type":"application/json"}:{},
      body:body?JSON.stringify(body):undefined,signal:ctl.signal});
    if(!r.ok) throw new Error(r.status);
    const j=await r.json();
    fails=0; $("live").classList.add("live");
    return j;
  }catch(e){
    fails++; $("live").classList.remove("live");
    return null;
  }finally{clearTimeout(bail);}
}

// Back off when the other end is not answering, rather than hammering it.
function schedule(){
  clearTimeout(timer);
  const wait = fails===0 ? 700 : Math.min(5000, 700*Math.pow(2,Math.min(fails,3)));
  timer=setTimeout(async()=>{
    // Whatever happens in there, the next tick is still booked. A single
    // exception used to end polling for the life of the page.
    try{ await poll(); }catch(e){ console.error(e); }
    finally{ schedule(); }
  },wait);
}

async function poll(){
  // Never redraw under someone's hand: a drag, or the gap inside a double
  // click, owns the layout.
  if(busy||dragging||Date.now()<holdRenderUntil){return;}
  const s=await api("/api/state");
  if(!s)return;
  const sig=JSON.stringify(s);
  if(sig===lastSig)return;   // unchanged, so leave the page alone entirely

  state=s; sources=s.sources||[];
  try{
    render();
    // Recorded only once the page really shows this state. Marking it before
    // drawing meant a single failed render was remembered as done, and every
    // later poll matched it and skipped, so the page stopped updating until
    // something forced it. Windows closed on the machine then sat there.
    lastSig=sig;
  }catch(e){
    lastSig="";
    console.error(e);
  }
}

function tagsFor(s){
  let h="";
  if(s.local) h+='<span class="tag local">this machine</span>';
  if(s.manual) h+='<span class="tag direct">direct</span>';
  if(s.mdns) h+='<span class="tag mdns">mDNS</span>';
  if(s.mini) h+='<span class="tag mini">OMT Mini</span>';
  if(s.manual&&s.offline) h+='<span class="tag off">offline</span>';
  return h;
}

function renderSources(){
  const box=$("sources");
  const list=sources.filter(s=>!filter||
    (s.name+" "+s.address).toLowerCase().includes(filter));
  if(!list.length){box.innerHTML='<div class="empty">No sources</div>';return;}
  box.innerHTML=list.map((s,i)=>
    `<div class="src" draggable="true" data-i="${i}">
       <div class="name">${esc(s.name)}${tagsFor(s)}</div>
       <div class="addr">${esc(s.address)}</div>
     </div>`).join("");
  [...box.querySelectorAll(".src")].forEach(el=>{
    const s=list[+el.dataset.i];
    el.addEventListener("dragstart",e=>{
      dragging={address:s.address,name:s.name};
      e.dataTransfer.setData("text/plain",s.address);
      e.dataTransfer.effectAllowed="copy";
    });
    el.addEventListener("dragend",()=>{dragging=null;});
  });
}

function esc(t){return (t||"").replace(/[&<>"]/g,c=>
  ({"&":"&amp;","<":"&lt;",">":"&gt;",'"':"&quot;"}[c]));}

function renderDesk(){
  const d=$("desk"), st=state;
  const dw=st.desktop.width||1920, dh=st.desktop.height||1080;
  const avail=d.parentElement.clientWidth-0;
  const scale=avail/dw;
  d.style.height=Math.round(dh*scale)+"px";
  $("deskinfo").textContent=dw+" x "+dh;

  const alive=new Set(st.windows.map(w=>w.id));
  [...closing].forEach(id=>{ if(!alive.has(id)) closing.delete(id); });
  [...optimistic.keys()].forEach(id=>{ if(!alive.has(id)) optimistic.delete(id); });

  d.innerHTML="";
  (st.monitors||[]).forEach(m=>{
    const e=document.createElement("div");
    e.className="mon";
    e.style.left=Math.round((m.x-st.desktop.x)*scale)+"px";
    e.style.top=Math.round((m.y-st.desktop.y)*scale)+"px";
    e.style.width=Math.round(m.width*scale)+"px";
    e.style.height=Math.round(m.height*scale)+"px";
    d.appendChild(e);
  });

  if(!st.windows.length){
    const e=document.createElement("div");
    e.className="empty";e.style.position="absolute";e.style.inset="0";
    e.style.display="flex";e.style.alignItems="center";e.style.justifyContent="center";
    e.textContent="Nothing open. Drag a source here to open a viewer.";
    d.appendChild(e);
  }

  st.windows.forEach(w=>{
    if(closing.has(w.id))return;

    // Prefer where the user put it until the machine reports the same thing,
    // or until it has clearly not taken.
    let g={x:w.x,y:w.y,width:w.width,height:w.height};
    const o=optimistic.get(w.id);
    if(o){
      const agreed=Math.abs(w.x-o.x)<3&&Math.abs(w.y-o.y)<3&&
                   Math.abs(w.width-o.width)<3&&Math.abs(w.height-o.height)<3;
      if(agreed||Date.now()>o.until) optimistic.delete(w.id);
      else g=o;
    }

    const e=document.createElement("div");
    e.className="win"+(w.kind==="multiview"?" mv":"")+(w.minimized?" min":"");
    e.style.left=Math.round((g.x-st.desktop.x)*scale)+"px";
    e.style.top=Math.round((g.y-st.desktop.y)*scale)+"px";
    e.style.width=Math.max(60,Math.round(g.width*scale))+"px";
    e.style.height=Math.max(38,Math.round(g.height*scale))+"px";
    e.innerHTML=
      `<div class="cap">${esc(w.title)}${w.kind==="multiview"?
         '<span class="tag direct">multiview</span>':""}</div>
       <div class="body">${w.minimized?"minimised":
         esc(w.address||(w.maximized?"maximised":""))}</div>
       <div class="m" title="${w.minimized?"Restore":"Minimise"}">${w.minimized?"&#9723;":"&minus;"}</div>
       <div class="x" title="Close">&times;</div><div class="grip"></div>`;
    d.appendChild(e);
    wireWindow(e,w,scale);
  });
}

function wireWindow(el,w,scale){
  el.querySelector(".x").addEventListener("click",async ev=>{
    ev.stopPropagation();
    // Gone from the page at once. The machine catches up in its own time, and
    // the mark is dropped when the window really has gone.
    closing.add(w.id);
    el.remove();
    busy=true; await api("/api/command",{kind:"close",id:w.id}); busy=false;
    lastSig=""; toast("Closed"); poll();
  });

  el.querySelector(".m").addEventListener("click",async ev=>{
    ev.stopPropagation();
    holdRenderUntil=Date.now()+300;
    busy=true;
    await api("/api/command",{kind:"state",id:w.id,
      state:w.minimized?"restore":"minimize"});
    busy=false; lastSig=""; holdRenderUntil=0; poll();
  });

  el.addEventListener("dblclick",async ev=>{
    ev.preventDefault();
    holdRenderUntil=Date.now()+400;
    busy=true;
    await api("/api/command",{kind:"state",id:w.id,
      state:w.maximized?"restore":"maximize"});
    busy=false; lastSig=""; holdRenderUntil=0; poll();
  });

  el.addEventListener("dragover",e=>{
    if(!dragging)return; e.preventDefault(); el.classList.add("drop");});
  el.addEventListener("dragleave",()=>el.classList.remove("drop"));
  el.addEventListener("drop",async e=>{
    e.preventDefault(); e.stopPropagation(); el.classList.remove("drop");
    if(!dragging)return;
    const a=dragging.address, k=w.kind; dragging=null;
    if(k==="multiview"){toast("Drop onto a multiview cell below");return;}
    busy=true; await api("/api/command",{kind:"source",id:w.id,address:a});
    busy=false; lastSig=""; toast("Source changed"); poll();
  });

  // move and resize, committed on release so the window is not spammed
  const grip=el.querySelector(".grip");
  let mode=null,sx=0,sy=0,ox=0,oy=0,ow=0,oh=0;
  const down=(e,m)=>{
    if(e.button!==0)return;
    // Covers the gap to a possible second click, so the element this one
    // landed on is still there for it.
    holdRenderUntil=Date.now()+450;
    mode=m;sx=e.clientX;sy=e.clientY;
    ox=parseFloat(el.style.left);oy=parseFloat(el.style.top);
    ow=parseFloat(el.style.width);oh=parseFloat(el.style.height);
    dragging="geom";e.preventDefault();e.stopPropagation();
    window.addEventListener("mousemove",move);
    window.addEventListener("mouseup",up);
  };
  const move=e=>{
    const dx=e.clientX-sx, dy=e.clientY-sy;
    if(mode==="move"){el.style.left=(ox+dx)+"px";el.style.top=(oy+dy)+"px";}
    else{el.style.width=Math.max(60,ow+dx)+"px";el.style.height=Math.max(38,oh+dy)+"px";}
  };
  const up=async()=>{
    window.removeEventListener("mousemove",move);
    window.removeEventListener("mouseup",up);
    const m=mode; mode=null; dragging=null;
    if(!m)return;
    const nx=Math.round(state.desktop.x+parseFloat(el.style.left)/scale);
    const ny=Math.round(state.desktop.y+parseFloat(el.style.top)/scale);
    const nw=Math.round(parseFloat(el.style.width)/scale);
    const nh=Math.round(parseFloat(el.style.height)/scale);
    // A click that moved nothing sends nothing, and must not poll either: a
    // redraw here would take the element out from under a double click.
    if(Math.abs(nx-w.x)<3&&Math.abs(ny-w.y)<3&&
       Math.abs(nw-w.width)<3&&Math.abs(nh-w.height)<3)return;

    optimistic.set(w.id,{x:nx,y:ny,width:nw,height:nh,until:Date.now()+1500});
    busy=true;
    await api("/api/command",{kind:"move",id:w.id,x:nx,y:ny,width:nw,height:nh});
    busy=false; lastSig=""; holdRenderUntil=0; poll();
  };
  el.addEventListener("mousedown",e=>{
    if(e.target.classList.contains("grip"))return;
    if(e.target.classList.contains("x"))return;
    if(e.target.classList.contains("m"))return;
    down(e,"move");
  });
  grip.addEventListener("mousedown",e=>down(e,"size"));
}

function renderTiles(){
  const sel=$("layout");
  if(sel.options.length!==LAYOUTS.length){
    sel.innerHTML=LAYOUTS.map((l,i)=>`<option value="${i}">${l.n}</option>`).join("");
  }
  sel.value=state.multiview_layout;

  const L=LAYOUTS[state.multiview_layout]||LAYOUTS[0];
  const box=$("tiles");
  box.style.gridTemplateColumns=`repeat(${L.c},1fr)`;
  box.style.gridTemplateRows=`repeat(${L.r},1fr)`;
  box.style.aspectRatio=(L.c*16)+" / "+(L.r*9);

  const tiles=state.multiview_tiles||[];
  box.innerHTML="";
  let n=0;
  const place=(col,row,span)=>{
    const i=n++;
    const a=tiles[i]||"";
    const e=document.createElement("div");
    e.className="tile";
    e.style.gridColumn=(col+1)+" / span "+span;
    e.style.gridRow=(row+1)+" / span "+span;
    e.innerHTML=a
      ? `<div class="t">${esc(shortName(a))}</div><div class="clr">clear</div>`
      : `<div class="e">empty</div>`;
    box.appendChild(e);
    e.addEventListener("dragover",ev=>{if(!dragging)return;
      ev.preventDefault();e.classList.add("drop");});
    e.addEventListener("dragleave",()=>e.classList.remove("drop"));
    e.addEventListener("drop",async ev=>{
      ev.preventDefault();e.classList.remove("drop");
      if(!dragging||dragging==="geom")return;
      const addr=dragging.address;dragging=null;
      busy=true;await api("/api/command",{kind:"tile",index:i,address:addr});
      busy=false;lastSig="";toast("Cell set");poll();
    });
    const clr=e.querySelector(".clr");
    if(clr)clr.addEventListener("click",async()=>{
      busy=true;await api("/api/command",{kind:"tile",index:i,address:""});
      busy=false;lastSig="";poll();
    });
  };
  if(L.h>0){
    place(0,0,L.h);
    for(let r=0;r<L.r;r++)for(let c=0;c<L.c;c++)
      if(c>=L.h||r>=L.h) place(c,r,1);
  }else{
    for(let r=0;r<L.r;r++)for(let c=0;c<L.c;c++) place(c,r,1);
  }
}

function renderMachines(){
  const box=$("machines");
  const list=state.machines||[];
  $("myver").textContent="this machine "+(state.version||"");
  if(!list.length){box.innerHTML='<div class="empty">No other OMT Mini found</div>';return;}

  box.innerHTML="";
  list.forEach(m=>{
    const behind=!m.current&&m.version;
    const e=document.createElement("div");
    e.className="mach"+(behind&&!m.self?" behind":"");
    e.innerHTML=
      `<span class="n">${esc(m.host)}</span>`+
      (m.self?'<span class="tag local">this machine</span>':"")+
      (behind?'<span class="tag beta">out of date</span>':"")+
      `<span class="v">${esc(m.version||"unknown")}${m.ip?"  "+esc(m.ip):""}</span>`+
      `<span class="sp"></span>`;
    box.appendChild(e);

    if(m.self)return;

    const b=document.createElement("button");
    if(!m.port){
      b.textContent="No control panel";
      b.disabled=true;
      b.title="That copy has its control panel turned off, so it cannot be asked.";
    }else if(!behind){
      b.textContent="Up to date";
      b.disabled=true;
    }else{
      b.textContent="Update "+m.host;
      b.className="warn";
      b.addEventListener("click",async()=>{
        // Deliberate, and spelled out: this closes windows on a machine the
        // person pressing it is probably not sitting at.
        const sure=confirm(
          "Update "+m.host+" now?\n\n"+
          "It is running "+(m.version||"an unknown version")+
          " and this machine is on "+state.version+".\n\n"+
          "That machine will close its viewers, stop its desktop capture and "+
          "multiview output, and restart.\n\nDo not do this if it is on air.");
        if(!sure)return;
        busy=true;
        const res=await fetch("/api/fleet/update",{method:"POST",
          headers:{"Content-Type":"application/json"},
          body:JSON.stringify({host:m.ip||m.host,port:m.port})}).then(x=>x.json()).catch(()=>null);
        busy=false; lastSig="";
        toast(res&&res.message?res.message:"Could not reach that machine");
        poll();
      });
    }
    e.appendChild(b);
  });
}

function shortName(a){
  const m=/^(.*) \((.*)\)$/.exec(a);
  return m?m[2]:a;
}

function render(){
  $("host").textContent=state.host||"";
  renderSources(); renderDesk(); renderMachines(); renderTiles();
}

$("search").addEventListener("input",e=>{filter=e.target.value.toLowerCase();renderSources();});
$("layout").addEventListener("change",async e=>{
  busy=true;await api("/api/command",{kind:"layout",index:+e.target.value});
  busy=false;lastSig="";poll();
});
$("mvopen").addEventListener("click",async()=>{
  busy=true;await api("/api/command",{kind:"openmv"});busy=false;
  lastSig="";toast("Multiview opened");poll();
});
$("addbtn").addEventListener("click",async()=>{
  const a=prompt("Address of the source to add.\nLeave the port off and every sender on that machine is found.");
  if(!a)return;
  busy=true;const r=await api("/api/command",{kind:"add",address:a});busy=false;
  lastSig="";toast(r&&r.message?r.message:"Added");poll();
});
$("desk").addEventListener("dragover",e=>{if(dragging&&dragging!=="geom")e.preventDefault();});
$("desk").addEventListener("drop",async e=>{
  e.preventDefault();
  if(!dragging||dragging==="geom")return;
  const a=dragging.address;dragging=null;
  busy=true;await api("/api/command",{kind:"open",address:a});busy=false;
  lastSig="";toast("Viewer opened");poll();
});
window.addEventListener("resize",()=>{if(state)renderDesk();});

poll(); schedule();
</script>
</body>
</html>
)HTMLPAGE";
