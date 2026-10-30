// SPDX-License-Identifier: GPL-3.0-or-later
const MAX_NAL=2*1024*1024;
function rbsp(nal){
  const out=[];
  for(let i=1;i<nal.length;i++){
    if(i>2&&nal[i]===3&&nal[i-1]===0&&nal[i-2]===0)continue;
    out.push(nal[i]);
  }
  return new Uint8Array(out);
}
class Bits{
  constructor(data){this.data=data;this.pos=0;}
  read(n){
    if(n>32||this.pos+n>this.data.length*8)throw new Error('Incomplete H.264 headers');
    let value=0;while(n--)value=value*2+((this.data[this.pos>>3]>>(7-(this.pos++&7)))&1);return value;
  }
  ue(){let zeros=0;while(!this.read(1)){if(++zeros>30)throw new Error('Invalid H.264 header');}return 2**zeros-1+this.read(zeros);}
  se(){const n=this.ue();return n&1?(n+1)/2:-n/2;}
}
export function avcDimensions(sps){
  const b=new Bits(rbsp(sps)),profile=b.read(8);b.read(16);b.ue();
  let chroma=1,separate=0;
  if([100,110,122,244,44,83,86,118,128,138,139,134,135].includes(profile)){
    chroma=b.ue();if(chroma>3)throw new Error('Invalid chroma format');
    if(chroma===3)separate=b.read(1);
    b.ue();b.ue();b.read(1);
    if(b.read(1))for(let i=0;i<(chroma===3?12:8);i++)if(b.read(1)){
      let last=8,next=8;for(let j=0;j<(i<6?16:64);j++){if(next)next=(last+b.se()+256)%256;last=next||last;}
    }
  }
  b.ue();const order=b.ue();
  if(order===0)b.ue();
  else if(order===1){b.read(1);b.se();b.se();const count=b.ue();if(count>256)throw new Error('Invalid picture order');for(let i=0;i<count;i++)b.se();}
  else if(order!==2)throw new Error('Invalid picture order');
  b.ue();b.read(1);const widthMbs=b.ue()+1,heightMap=b.ue()+1,frame=b.read(1);
  if(!frame)b.read(1);b.read(1);
  let left=0,right=0,top=0,bottom=0;
  if(b.read(1)){left=b.ue();right=b.ue();top=b.ue();bottom=b.ue();}
  const format=separate?0:chroma;
  const x=format===1||format===2?2:1,y=(2-frame)*(format===1?2:1);
  const width=widthMbs*16-(left+right)*x,height=heightMap*16*(2-frame)-(top+bottom)*y;
  if(width<1||height<1||width>8192||height>8192)throw new Error('Invalid recording dimensions');
  let fps;
  if(b.read(1)){
    if(b.read(1)){const aspect=b.read(8);if(aspect===255)b.read(32);}
    if(b.read(1))b.read(1);
    if(b.read(1)){b.read(4);if(b.read(1))b.read(24);}
    if(b.read(1)){b.ue();b.ue();}
    if(b.read(1)){
      const units=b.read(32),scale=b.read(32);b.read(1);
      const value=scale/(2*units);if(Number.isFinite(value)&&value>=1&&value<=240)fps=value;
    }
  }
  return {width,height,...(fps?{fps}:{})};
}
const hex=bytes=>Array.from(bytes,v=>v.toString(16).padStart(2,'0')).join('');
export class AvcRecording{
  constructor(){this.pending=new Uint8Array();this.units=[];this.vcl=false;this.sps=null;this.pps=null;}
  get config(){
    if(!this.sps||!this.pps)return null;
    return {...avcDimensions(this.sps),sps:hex(this.sps),pps:hex(this.pps),codec:'avc1.'+hex(this.sps.subarray(1,4))};
  }
  push(chunk,final=false){
    const all=new Uint8Array(this.pending.length+chunk.length);all.set(this.pending);all.set(chunk,this.pending.length);
    const starts=[];
    for(let i=0;i+2<all.length;i++)if(!all[i]&&!all[i+1]){
      if(all[i+2]===1){starts.push([i,i+3]);i+=2;}
      else if(i+3<all.length&&!all[i+2]&&all[i+3]===1){starts.push([i,i+4]);i+=3;}
    }
    const frames=[];
    for(let i=0;i<starts.length-(final?0:1);i++)this.nal(all.subarray(starts[i][1],starts[i+1]?.[0]??all.length),frames);
    this.pending=final?new Uint8Array():all.slice(starts.at(-1)?.[0]??0);
    if(this.pending.length>MAX_NAL)throw new Error('Recording contains an oversized H.264 unit');
    if(final)this.flush(frames);
    return frames;
  }
  flush(frames){
    if(!this.vcl)return;
    const size=this.units.reduce((n,unit)=>n+4+unit.length,0);if(size>MAX_NAL)throw new Error('Recording frame is too large');
    const data=new Uint8Array(size);let offset=0;
    for(const unit of this.units){data.set([0,0,0,1],offset);data.set(unit,offset+4);offset+=4+unit.length;}
    frames.push(data);this.units=[];this.vcl=false;
  }
  nal(nal,frames){
    if(!nal.length)return;
    if(nal.length>MAX_NAL)throw new Error('Recording unit is too large');
    const type=nal[0]&31;
    const picture=type===1||type===5;
    const first=picture&&new Bits(rbsp(nal.subarray(0,32))).ue()===0;
    if(this.vcl&&(type===9||type===7||type===8||type===6||first))this.flush(frames);
    if(type===7)this.sps=nal.slice();if(type===8)this.pps=nal.slice();
    this.units.push(nal.slice());if(picture)this.vcl=true;
    if(this.units.length>512||this.units.reduce((n,u)=>n+u.length+4,0)>MAX_NAL)throw new Error('Recording has too much data without a picture');
  }
}
