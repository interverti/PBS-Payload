/* Minimal ISO BMFF muxer: Annex B AVC access units -> fragmented MP4. */
const H264Mux = (() => {
  const bytes = (...v) => new Uint8Array(v.flat());
  const join = (...arrays) => { const out = new Uint8Array(arrays.reduce((n,a)=>n+a.length,0)); let pos=0; for(const a of arrays){out.set(a,pos);pos+=a.length;} return out; };
  const str = s => new TextEncoder().encode(s);
  const u16 = n => bytes(n>>>8,n&255);
  const u32 = n => bytes(n>>>24,(n>>>16)&255,(n>>>8)&255,n&255);
  const zeros = n => new Uint8Array(n);
  const box = (type,...data) => {const body=join(...data);return join(u32(body.length+8),str(type),body);};
  const full = (type,version,flags,...data) => box(type,bytes(version,flags>>>16,(flags>>>8)&255,flags&255),...data);
  const matrix = join(u32(0x10000),u32(0),u32(0),u32(0),u32(0x10000),u32(0),u32(0),u32(0),u32(0x40000000));
  const hex = s => new Uint8Array(s.match(/../g).map(x=>parseInt(x,16)));
  function init(config) {
    const sps=hex(config.sps),pps=hex(config.pps),width=config.width,height=config.height;
    const avcc=box('avcC',bytes(1,sps[1],sps[2],sps[3],255,225),u16(sps.length),sps,bytes(1),u16(pps.length),pps);
    const avc1=box('avc1',zeros(6),u16(1),zeros(16),u16(width),u16(height),u32(0x480000),u32(0x480000),u32(0),u16(1),zeros(32),u16(24),u16(65535),avcc);
    const stbl=box('stbl',full('stsd',0,0,u32(1),avc1),full('stts',0,0,u32(0)),full('stsc',0,0,u32(0)),full('stsz',0,0,u32(0),u32(0)),full('stco',0,0,u32(0)));
    const dinf=box('dinf',full('dref',0,0,u32(1),full('url ',0,1)));
    const minf=box('minf',full('vmhd',0,1,zeros(8)),dinf,stbl);
    const mdia=box('mdia',full('mdhd',0,0,u32(0),u32(0),u32(90000),u32(0),u16(0x55c4),u16(0)),full('hdlr',0,0,u32(0),str('vide'),zeros(12),str('PBS video\0')),minf);
    const trak=box('trak',full('tkhd',0,7,u32(0),u32(0),u32(1),u32(0),u32(0),zeros(8),u16(0),u16(0),u16(0),u16(0),matrix,u32(width*65536),u32(height*65536)),mdia);
    const mvhd=full('mvhd',0,0,u32(0),u32(0),u32(90000),u32(0),u32(0x10000),u16(0x100),zeros(10),matrix,zeros(24),u32(2));
    const duration=Math.round(90000/(config.fps||60));
    const mvex=box('mvex',full('trex',0,0,u32(1),u32(1),u32(duration),u32(0),u32(0)));
    return join(box('ftyp',str('isom'),u32(512),str('isomiso6avc1mp41')),box('moov',mvhd,trak,mvex));
  }
  function accessUnit(data) {
    const starts=[];
    for(let i=0;i+3<data.length;i++){
      if(data[i]===0&&data[i+1]===0){
        if(data[i+2]===1){starts.push([i,i+3]);i+=2;}
        else if(data[i+2]===0&&data[i+3]===1){starts.push([i,i+4]);i+=3;}
      }
    }
    if(!starts.length)throw new Error('Unrecognized stream format');
    let key=false; const nals=[];
    for(let i=0;i<starts.length;i++){
      const end=i+1<starts.length?starts[i+1][0]:data.length;
      const nal=data.subarray(starts[i][1],end);if(!nal.length)continue;
      if((nal[0]&31)===5)key=true;
      nals.push(u32(nal.length),nal);
    }
    return {data:join(...nals),key};
  }
  function fragment(sequence,pts,samples) {
    const mdat=box('mdat',...samples.map(s=>s.data));
    const makeMoof=offset=>box('moof',full('mfhd',0,0,u32(sequence)),box('traf',full('tfhd',0,0x020000,u32(1)),full('tfdt',1,0,u32(Math.floor(pts/4294967296)),u32(pts)),full('trun',0,0x701,u32(samples.length),u32(offset),...samples.map(s=>join(u32(s.duration||1500),u32(s.data.length),u32(s.key?0x02000000:0x01010000))))));
    const moof=makeMoof(0); return join(makeMoof(moof.length+8),mdat);
  }
  return {init,accessUnit,fragment};
})();
