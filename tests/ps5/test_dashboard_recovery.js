// Test the browser viewer without a console by holding /frames open forever.
const assert=require('node:assert/strict');
const fs=require('node:fs');
const vm=require('node:vm');
const html=fs.readFileSync(require('node:path').join(__dirname,'../../payload/web/h264.html'),'utf8');
const scripts=[...html.matchAll(/<script>([\s\S]*?)<\/script>/g)].map(m=>m[1]);
assert.equal(scripts.length,3);
let now=1000,frameConnections=0,opened=0,closed=0,lastSource=null;
const timers=new Map();
let nextTimer=1,currentMedia=null;
class Events{
 constructor(){this.listeners=new Map();}
 addEventListener(name,fn){if(!this.listeners.has(name))this.listeners.set(name,new Set());this.listeners.get(name).add(fn);}
 removeEventListener(name,fn){this.listeners.get(name)?.delete(fn);}
 emit(name){for(const fn of this.listeners.get(name)||[])fn({type:name});}
}
class FakeSource extends Events{
 constructor(){super();this.updating=false;}
 appendBuffer(){this.updating=true;setImmediate(()=>{this.updating=false;this.emit('updateend');});}
 remove(){this.emit('updateend');}
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
 }
 nodes.set(id,el);return el;
}
const document={hidden:false,activeElement:null,addEventListener(){},createElement(){return node('created');},getElementById:node};
const window=new Events();
window.MediaSource=FakeMedia;
const url={createObjectURL(media){currentMedia=media;opened++;return 'blob:test-'+opened;},revokeObjectURL(){closed++;}};
function fetchMock(path,options={}){
 if(path==='/frames'){
  frameConnections++;
  let rejectRead=null;
  const cancel=()=>{if(rejectRead)rejectRead(new Error('request cancelled'));};
  options.signal?.addEventListener('abort',cancel,{once:true});
  return Promise.resolve({ok:true,body:{getReader(){return{
   read(){return new Promise((resolve,reject)=>{rejectRead=reject;if(options.signal?.aborted)reject(new Error('aborted'));});},
   cancel(){return Promise.resolve();}
  };}}});
 }
 if(path==='/status.json'){
  return Promise.resolve({ok:true,json:async()=>({recording:false,recording_name:'',recording_bytes:0,
   recording_seconds:0,width:1920,height:1080,fps:60,kbps:5000})});
 }
 if(path==='/recordings')return Promise.resolve({ok:true,json:async()=>[]});
 if(path==='/config.json')return Promise.resolve({ok:true,json:async()=>({
  codec:'avc1.64002A',platform:'PS5',width:1920,height:1080,fps:60,resolutions:[]
 })});
 return Promise.resolve({ok:true,json:async()=>({})});
}
const sandbox={window,document,MediaSource:FakeMedia,URL:url,fetch:fetchMock,
 Uint8Array,DataView,AbortController,Option:function(){},Math,console,
 Date:{now:()=>now},H264Mux:{init:()=>new Uint8Array([1]),fragment:()=>new Uint8Array([2]),accessUnit:()=>({data:new Uint8Array([3]),key:true})},
 setInterval(fn){const id=nextTimer++;timers.set(id,fn);return id;},
 clearInterval(id){timers.delete(id);},
 setTimeout(fn){return setTimeout(fn,25);},clearTimeout,Promise};
vm.runInNewContext(scripts[1],sandbox,{filename:'dashboard.js'});
async function settle(){for(let i=0;i<12;i++)await new Promise(r=>setImmediate(r));}
(async()=>{
 await settle();
 assert.equal(frameConnections,1,'first video connection must open');
 now+=10000;
 for(const fn of [...timers.values()])fn();
 await new Promise(r=>setTimeout(r,120));
 await settle();
 assert.ok(frameConnections>=2,'watchdog should reopen the hung /frames fetch without reloading page');
 assert.ok(closed>=1,'previous MediaSource URL should be released');
 const previous=frameConnections;
 lastSource.emit('error');
 await new Promise(r=>setTimeout(r,120));
 await settle();
 assert.ok(frameConnections>previous,'async SourceBuffer error should trigger recovery');
 const before=frameConnections;
 await node('stop').onclick();
 await new Promise(r=>setTimeout(r,100));
 await settle();
 assert.equal(frameConnections,before,'Stop must prevent reconnecting');
 const aqCode=scripts[2].slice(scripts[2].indexOf('const AQ='),scripts[2].indexOf('function pcmToFloat'));
 const audioSandbox={Float32Array};
 vm.runInNewContext(aqCode+';globalThis.audioQueue=AQ;',audioSandbox);
 const aq=audioSandbox.audioQueue;
 for(let i=0;i<800;i++)aq.push(new Float32Array(960),2);
 assert.ok(aq.recv-aq.head<=aq.rate*2,'audio queue must stay capped in background');
 assert.ok(aq.chunks.length<220,'audio queue should release old buffers');
 aq.pull([new Float32Array(2048),new Float32Array(2048)],2048);
 console.log('PASS: stalled frames reconnect; SourceBuffer errors recover; Stop prevents retry; audio queue bounded');
})().catch(e=>{console.error(e);process.exit(1);});