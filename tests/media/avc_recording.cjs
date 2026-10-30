// Offline H.264 recording parser/remux regression. Requires ffmpeg and ffprobe.
const assert=require('node:assert/strict'),fs=require('node:fs'),path=require('node:path'),vm=require('node:vm');
const {spawnSync}=require('node:child_process'),{pathToFileURL}=require('node:url');
const root=path.resolve(__dirname,'../..'),out=path.join(root,'build/issue-tests/avc');fs.mkdirSync(out,{recursive:true});
const ffmpeg=process.env.FFMPEG||'ffmpeg',ffprobe=process.env.FFPROBE||'ffprobe';
function run(bin,args){const r=spawnSync(bin,args,{encoding:'utf8'});if(r.error)throw r.error;assert.equal(r.status,0,r.stderr);return r.stdout;}
(async()=>{
 const {AvcRecording}=await import(pathToFileURL(path.join(root,'payload/web/avc-recording.js')));
 const ctx={Uint8Array,DataView,TextEncoder};vm.runInNewContext(fs.readFileSync(path.join(root,'payload/web/h264-mux.js'),'utf8')+';globalThis.mux=H264Mux;',ctx);
 for(const [name,profile,aud,slices,fps]of[['baseline','baseline',1,1,60],['high-no-aud','high',0,1,60],['multislice','high',0,4,60],['legacy-30fps','high',1,1,30]]){
  const rawPath=path.join(out,name+'.h264');
  run(ffmpeg,['-hide_banner','-loglevel','error','-y','-f','lavfi','-i','testsrc2=size=320x180:rate='+fps,'-frames:v','12','-c:v','libx264','-preset','fast','-profile:v',profile,'-pix_fmt','yuv420p','-x264-params',`aud=${aud}:slices=${slices}:keyint=6:min-keyint=6:scenecut=0:bframes=0`,'-f','h264',rawPath]);
  const bytes=fs.readFileSync(rawPath);let frames,parser;
  for(const chunkSize of [7,1009,262144]){
   parser=new AvcRecording();frames=[];
   for(let i=0;i<bytes.length;i+=chunkSize)frames.push(...parser.push(bytes.subarray(i,i+chunkSize)));
   frames.push(...parser.push(new Uint8Array(),true));
   assert.equal(frames.length,12,name+' frame grouping with chunk size '+chunkSize);
   assert.equal(parser.config.width,320);assert.equal(parser.config.height,180);assert.equal(parser.config.fps,fps);
  }
  const {mux}=ctx;const data=[mux.init({...parser.config,fps})];
  for(let i=0;i<frames.length;i++){const sample=mux.accessUnit(frames[i]);data.push(mux.fragment(i+1,i*90000/fps,[{data:sample.data,duration:90000/fps,key:sample.key}]));}
  const movie=path.join(out,name+'.mp4');fs.writeFileSync(movie,Buffer.concat(data));
  run(ffmpeg,['-hide_banner','-loglevel','error','-i',movie,'-f','null','-']);
  const info=JSON.parse(run(ffprobe,['-v','error','-count_frames','-show_entries','stream=width,height,nb_read_frames','-of','json',movie]));
  assert.equal(Number(info.streams[0].nb_read_frames),12);assert.equal(info.streams[0].height,180);
 }
 assert.throws(()=>new AvcRecording().push(new Uint8Array(2*1024*1024+1)),/oversized/);
 console.log('PASS: recording dimensions/cropping, AUD and no-AUD frame grouping, multi-slice frames, split NAL prefixes, bounded parsing and MP4 decoding');
})().catch(e=>{console.error(e);process.exitCode=1;});
