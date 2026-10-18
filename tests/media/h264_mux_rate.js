// SPDX-License-Identifier: GPL-3.0-or-later
const fs=require('fs'),vm=require('vm'),assert=require('assert');
const context={TextEncoder,Uint8Array};
vm.runInNewContext(fs.readFileSync('payload/web/h264-mux.js','utf8')+';globalThis.mux=H264Mux',context);
for(const fps of [60,30]){
 const duration=90000/fps;
 const init=Buffer.from(context.mux.init({width:1920,height:1080,fps,sps:'674d4028',pps:'68ee3c80'}));
 const trex=init.indexOf(Buffer.from('trex'));assert(trex>=0);assert.equal(init.readUInt32BE(trex+16),duration);
 const fragment=Buffer.from(context.mux.fragment(1,0,[{data:new Uint8Array([1,2,3]),key:true,duration}]));
 const trun=fragment.indexOf(Buffer.from('trun'));assert(trun>=0);assert.equal(fragment.readUInt32BE(trun+16),duration);
}
console.log('Browser MP4: 60fps/30fps init and sample durations verified');
