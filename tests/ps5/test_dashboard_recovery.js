// Test the browser viewer without a console by holding /frames open forever.
const assert=require('node:assert/strict');
const fs=require('node:fs');
const vm=require('node:vm');
const html=fs.readFileSync(require('node:path').join(__dirname,'../../payload/web/h264.html'),'utf8');
const scripts=[...html.matchAll(/<script>([\s\S]*?)<\/script>/g)].map(m=>m[1]);
assert.equal(scripts.length,3);
let now=1000,frameConnections=0,opened=0,closed=0,lastSource=null;
let deliverFrame=null,removed=[],fragmentTimes=[],cancelledCallbacks=0,keyRequests=0;
const timers=new Map();
let nextTimer=1,currentMedia=null;
class Events{
 constructor(){this.listeners=new Map();}
 addEventListener(name,fn){if(!this.listeners.has(name))this.listeners.set(name,new Set());this.listeners.get(name).add(fn);}
 removeEventListener(name,fn){this.listeners.get(name)?.delete(fn);}
 emit(name){for(const fn of this.listeners.get(name)||[])fn({type:name});}
}
class FakeSource extends Events{
 constructor(){super();this.updating=false;this.keys=[];this.removedPlayhead=false;}
 appendBuffer(data){
  if(data.length===10&&data[8])this.keys.push(new DataView(data.buffer).getFloat64(0));
  this.updating=true;setImmediate(()=>{this.updating=false;this.emit('updateend');});
 }
 remove(start,end){
  // MSE coded-frame removal extends to the next IDR, or all remaining
  // media if there is no later IDR. Model that instead of a no-op remove.
  const actualEnd=this.keys.find(key=>key>=end)??Infinity;
  if(node('video').currentTime>=start&&node('video').currentTime<actualEnd)this.removedPlayhead=true;
  removed.push([start,end]);this.updating=true;setImmediate(()=>{this.updating=false;this.emit('updateend');});
 }
}
class FakeMedia extends Events{
 static isTypeSupported(){return true;}
 addSourceBuffer(){lastSource=new FakeSource();return lastSource;}
}
const nodes=new Map();
function node(id){
 if(nodes.has(id))return nodes.get(id);
 const el=new Events();
 Object.assign(el,{
  textContent:'',hidden:false,disabled:false,value:'120',dataset:{},classList:{toggle(){},add(){},remove(){}},
  replaceChildren(){},appendChild(){},pause(){this.paused=true;},
  play(){this.paused=false;this.emit('playing');return Promise.resolve();},
  load(){},removeAttribute(){},buffered:{length:0},paused:true,currentTime:0,videoWidth:1920
 });
 if(id==='video'){
  Object.defineProperty(el,'src',{set(){setImmediate(()=>{currentMedia?.emit('sourceopen');});}});
  el.requestVideoFrameCallback=()=>1;
  el.cancelVideoFrameCallback=()=>{cancelledCallbacks++;};
 }
 nodes.set(id,el);return el;
}
const document=Object.assign(new Events(),{hidden:false,activeElement:null,createElement(){return node('created');},getElementById:node});
const window=new Events();
window.MediaSource=FakeMedia;
const url={createObjectURL(media){currentMedia=media;opened++;return 'blob:test-'+opened;},revokeObjectURL(){closed++;}};
function fetchMock(path,options={}){
 if(path==='/video/keyframe'){keyRequests++;return Promise.resolve(jsonResponse({}));}
 if(path==='/frames'){
  frameConnections++;
  let rejectRead=null;
  const cancel=()=>{if(rejectRead)rejectRead(new Error('request cancelled'));};
  options.signal?.addEventListener('abort',cancel,{once:true});
  return Promise.resolve({ok:true,body:{getReader(){return{
   read(){return new Promise((resolve,reject)=>{deliverFrame=resolve;rejectRead=reject;if(options.signal?.aborted)reject(new Error('aborted'));});},
   cancel(){return Promise.resolve();}
  };}}});
 }
 if(path==='/status.json'){
  return Promise.resolve(jsonResponse({recording:false,recording_name:'',recording_bytes:0,
   recording_seconds:0,width:1920,height:1080,fps:60,kbps:5000}));
 }
 if(path==='/recordings')return Promise.resolve(jsonResponse([]));
 if(path==='/config.json')return Promise.resolve(jsonResponse({
  codec:'avc1.64002A',platform:'PS5',width:1920,height:1080,fps:60,resolutions:[]
 }));
 return Promise.resolve(jsonResponse({}));
}
function jsonResponse(value){return {ok:true,json:async()=>value,text:async()=>JSON.stringify(value)};}
const sandbox={window,document,MediaSource:FakeMedia,URL:url,fetch:fetchMock,
 Uint8Array,DataView,AbortController,Option:function(){},Math,console,
 Date:{now:()=>now},H264Mux:{init:()=>new Uint8Array([1]),fragment:(seq,pts,samples)=>{
  fragmentTimes.push(pts);const data=new Uint8Array(10);new DataView(data.buffer).setFloat64(0,pts/90000);data[8]=samples[0].key?1:0;return data;
 },accessUnit:data=>({data:new Uint8Array([3]),key:(data[0]&31)===5})},
 setInterval(fn){const id=nextTimer++;timers.set(id,fn);return id;},
 clearInterval(id){timers.delete(id);},
 setTimeout(fn){return setTimeout(fn,25);},clearTimeout,Promise};
vm.runInNewContext(scripts[1],sandbox,{filename:'dashboard.js'});
async function settle(){for(let i=0;i<12;i++)await new Promise(r=>setImmediate(r));}
(async()=>{
 await settle();
 assert.equal(frameConnections,1,'first video connection must open');
 // Deliver several frames together while appendBuffer is busy. Trimming
 // must run with queued fragments and mild jitter must not force a seek.
 const video=node('video');
 // Do not start on just two or three frames and immediately underflow.
 video.buffered={length:1,start:()=>0,end:()=>0.05};
 lastSource.emit('updateend');await settle();
 assert.equal(video.paused,true,'startup must accumulate a jitter cushion before playing');
 video.currentTime=170;
 video.buffered={length:1,start:()=>0,end:()=>170.15};
 const packet=new Uint8Array(27),view=new DataView(packet.buffer);
 for(let i=0;i<3;i++){view.setUint32(i*9,1);view.setUint32(i*9+4,(0xfffffa24+i*1500)>>>0);packet[i*9+8]=i===0?0x65:0x41;}
 deliverFrame({value:packet,done:false});
 await settle();
 assert.deepEqual(fragmentTimes,[0,1500,3000],'wire clock must rebase and unwrap across uint32 rollover');
 assert.deepEqual(removed,[],'a long GOP with only its initial IDR must never be trimmed through the playhead');
 const safePacket=new Uint8Array(27),safeView=new DataView(safePacket.buffer);
 for(let i=0;i<3;i++){safeView.setUint32(i*9,1);safeView.setUint32(i*9+4,(0xfffffa24+13500000+i*1500)>>>0);safePacket[i*9+8]=i===0?0x65:0x41;}
 deliverFrame({value:safePacket,done:false});await settle();
 assert.deepEqual(removed,[[0,149.999]],'trim only before an appended IDR, also with queued fragments');
 assert.equal(lastSource.removedPlayhead,false,'MSE dependency removal must preserve the current picture');
 assert.equal(video.currentTime,170,'150 ms network jitter must not trigger a seek');
 assert.equal(video.playbackRate,1,'normal playback cushion should use normal speed');
 now+=3000;for(const fn of [...timers.values()])fn();await settle();
 assert.ok(keyRequests>0,'viewer must request periodic IDRs when the encoder emits none');
 now+=10000;
 for(const fn of [...timers.values()])fn();
 await new Promise(r=>setTimeout(r,120));
 await settle();
 assert.ok(frameConnections>=2,'watchdog should reopen the hung /frames fetch without reloading page');
 assert.ok(closed>=1,'previous MediaSource URL should be released');
 assert.ok(cancelledCallbacks>=1,'previous video frame callback must be cancelled');
 const previous=frameConnections;
 lastSource.emit('error');
 await new Promise(r=>setTimeout(r,120));
 await settle();
 assert.ok(frameConnections>previous,'async SourceBuffer error should trigger recovery');
 async function expectRecovery(label,prepare,elapsed=13000,beforeWatch=()=>{}){
  const before=frameConnections;
  const restore=prepare();
  now+=elapsed;
  // Network and capture stay healthy while the browser stops presenting.
  const bytes=new Uint8Array(9),header=new DataView(bytes.buffer);
  header.setUint32(0,1);header.setUint32(4,0);bytes[8]=0x65;
  deliverFrame({value:bytes,done:false});
  await settle();
  beforeWatch();
  for(const fn of [...timers.values()])fn();
  if(restore)restore();
  await new Promise(r=>setTimeout(r,120));await settle();
  assert.ok(frameConnections>before,label);
 }
 await expectRecovery('browser-paused video must recover while packets keep arriving',()=>{
  video.paused=true;const original=video.play;video.play=()=>new Promise(()=>{});
  return ()=>{video.play=original;};
 });
 await expectRecovery('a stuck SourceBuffer must recover even with only one queued fragment',()=>{
  lastSource.updating=true;
 },8000);
 await expectRecovery('playing events without presented frames must not mask a frozen decoder',()=>{
  video.paused=false;
 },13000,()=>video.emit('playing'));
 const backgroundBefore=frameConnections;
 document.hidden=true;document.emit('visibilitychange');
 now+=6000;
 document.hidden=false;document.emit('visibilitychange');
 await new Promise(r=>setTimeout(r,120));await settle();
 assert.ok(frameConnections>backgroundBefore,'returning from a short suspension must reconnect without a page reload');
 const before=frameConnections;
 await node('stop').onclick();
 await new Promise(r=>setTimeout(r,100));
 await settle();
 assert.equal(frameConnections,before,'Stop must prevent reconnecting');
 // A response that sends headers but never completes JSON must time out.
 sandbox.fetch=(path,options)=>new Promise(resolve=>resolve({ok:true,text:()=>new Promise((res,reject)=>{
  options.signal.addEventListener('abort',()=>reject(new Error('body timed out')),{once:true});
 })}));
 await assert.rejects(vm.runInNewContext("fetchWithTimeout('/hung-json')",sandbox),/body timed out/);
 const aqCode=scripts[2].slice(scripts[2].indexOf('const AQ='),scripts[2].indexOf('function pcmToFloat'));
 const audioSandbox={Float32Array};
 vm.runInNewContext(aqCode+';globalThis.audioQueue=AQ;',audioSandbox);
 const aq=audioSandbox.audioQueue;
 for(let i=0;i<800;i++)aq.push(new Float32Array(960),2);
 assert.ok(aq.recv-aq.head<=aq.rate*2,'audio queue must stay capped in background');
 assert.ok(aq.chunks.length<220,'audio queue should release old buffers');
 aq.pull([new Float32Array(2048),new Float32Array(2048)],2048);
 console.log('PASS: paused playback and pending play recover; frozen decoder and SourceBuffer recover; short background resume; timestamp rollover; trimming; jitter cushion; cleanup; JSON timeout; Stop; bounded audio');
})().catch(e=>{console.error(e);process.exit(1);});
