// SPDX-License-Identifier: GPL-3.0-or-later
import {AvcRecording} from './avc-recording.js';

export function installPreview(mux,live){
  const dialog=document.getElementById('preview-dialog'),content=document.getElementById('preview-content');
  const status=document.getElementById('preview-status'),download=document.getElementById('preview-download');
  let controller=null,url=null,clip=null,resumeLive=false;
  function release(){
    controller?.abort();controller=null;
    if(clip){clip.pause();clip.removeAttribute('src');clip.load();clip=null;}
    content.replaceChildren();if(url)URL.revokeObjectURL(url);url=null;
    if(resumeLive&&!live.isStopped()){live.resume();document.getElementById('video').play().catch(()=>{});}
    resumeLive=false;
  }
  function open(title){
    release();document.getElementById('preview-title').textContent=title;status.textContent='';
    if(!dialog.open)dialog.showModal();
  }
  dialog.addEventListener('close',release);
  document.getElementById('close-preview').onclick=()=>dialog.close();
  async function previewRecording(name,directory){
    open(name);controller=new AbortController();const {signal}=controller;
    resumeLive=!live.isPaused()&&!document.getElementById('video').paused;
    if(resumeLive){live.pause();document.getElementById('video').pause();}
    const recordingUrl='/recordings/'+encodeURIComponent(name)+'?directory='+encodeURIComponent(directory);
    download.href=recordingUrl;download.download=name;
    clip=document.createElement('video');clip.muted=true;clip.playsInline=true;
    const player=document.createElement('pbs-preview-player'),skin=document.createElement('video-skin');
    skin.appendChild(clip);player.appendChild(skin);content.appendChild(player);
    const video=clip;status.textContent='Loading video preview...';
    let source=null,media=null,index=0,keys=[],lastTrim=0;
    async function transfer(address,options={},json=false){
      const request=new AbortController(),cancel=()=>request.abort();
      const timer=setTimeout(cancel,20000);signal.addEventListener('abort',cancel,{once:true});
      if(signal.aborted)cancel();
      try{
        const response=await fetch(address,{...options,signal:request.signal,cache:'no-store'});
        const data=json?await response.json():response.status===206?await response.arrayBuffer():await response.text();
        return {ok:response.ok,status:response.status,headers:response.headers,data};
      }catch(e){if(request.signal.aborted&&!signal.aborted)throw new Error('Recording transfer timed out. Try again.');throw e;}
      finally{clearTimeout(timer);signal.removeEventListener('abort',cancel);}
    }
    const wait=(ms)=>new Promise((resolve,reject)=>{
      const cancel=()=>{clearTimeout(timer);reject(new Error('Preview closed'));};
      const timer=setTimeout(()=>{signal.removeEventListener('abort',cancel);resolve();},ms);
      signal.addEventListener('abort',cancel,{once:true});if(signal.aborted)cancel();
    });
    function update(action){return new Promise((resolve,reject)=>{
      const clean=()=>{clearTimeout(timer);source.removeEventListener('updateend',done);source.removeEventListener('error',failed);signal.removeEventListener('abort',failed);};
      const done=()=>{clean();resolve();};
      const failed=()=>{clean();reject(new Error('Preview buffer interrupted'));};
      const timer=setTimeout(failed,10000);
      source.addEventListener('updateend',done,{once:true});source.addEventListener('error',failed,{once:true});signal.addEventListener('abort',failed,{once:true});
      if(signal.aborted){failed();return;}try{action();}catch(e){clean();reject(e);}
    });}
    try{
      let metadata={};
      const info=await transfer('/recordings/info?name='+encodeURIComponent(name),{},true);
      if(!info.ok)throw new Error('Stop the recording before opening its preview.');
      metadata=info.data;let fps=[30,60].includes(metadata.fps)?metadata.fps:null;
      const parser=new AvcRecording();let offset=0,total=null,started=false;
      async function append(frame){
        if(signal.aborted)throw new Error('Preview closed');
        if(!source){
          const config=parser.config;if(!config)throw new Error('Recording has no H.264 headers');
          const estimated=!fps&&!config.fps;fps=fps||config.fps||60;
          const mime='video/mp4; codecs="'+config.codec+'"';
          const Media=[window.MediaSource,window.ManagedMediaSource].find(M=>M?.isTypeSupported(mime));
          if(!Media)throw new Error('This browser cannot preview the recording. Download it instead.');
          if(Media===window.ManagedMediaSource)video.disableRemotePlayback=true;
          media=new Media();url=URL.createObjectURL(media);video.src=url;
          await new Promise((resolve,reject)=>{
            const clean=()=>{clearTimeout(timer);media.removeEventListener('sourceopen',opened);media.removeEventListener('error',failed);signal.removeEventListener('abort',failed);};
            const opened=()=>{clean();resolve();};const failed=()=>{clean();reject(new Error('Preview could not start'));};
            const timer=setTimeout(failed,15000);
            media.addEventListener('sourceopen',opened,{once:true});media.addEventListener('error',failed,{once:true});signal.addEventListener('abort',failed,{once:true});if(signal.aborted)failed();
          });
          source=media.addSourceBuffer(mime);await update(()=>source.appendBuffer(mux.init({...config,fps})));
          status.textContent=estimated?'Video preview · legacy recording timing estimated at 60 fps':'Video preview · audio is not included in H.264 recordings';
        }
        // Fetch only small HTTP ranges, then pause fetching when sufficiently
        // buffered. A paused tab never holds a blocked file-download worker.
        while(!signal.aborted&&video.buffered.length&&video.buffered.end(video.buffered.length-1)-video.currentTime>15)await wait(200);
        const sample=mux.accessUnit(frame),pts=Math.round(index*90000/fps),duration=Math.round((index+1)*90000/fps)-pts;
        await update(()=>source.appendBuffer(mux.fragment(index+1,pts,[{data:sample.data,duration,key:sample.key}])));
        if(sample.key)keys.push(pts/90000);index++;
        if(!started&&index>=6){started=true;video.play().catch(()=>{status.textContent='Press Play to preview the recording.';});}
        if(video.currentTime-lastTrim>5){
          const boundary=keys.filter(key=>key<video.currentTime-10).at(-1);
          if(boundary>0){await update(()=>source.remove(0,boundary-0.001));keys=keys.filter(key=>key>=boundary);lastTrim=video.currentTime;}
        }
      }
      while(!signal.aborted&&(total===null||offset<total)){
        const range=await transfer(recordingUrl,{headers:{Range:'bytes='+offset+'-'+(offset+262143)}});
        if(range.status!==206){
          let message='Unable to load this recording. Reload the recordings list.';
          try{message=JSON.parse(range.data).message||message;}catch{}
          throw new Error(message);
        }
        const match=/^bytes (\d+)-(\d+)\/(\d+)$/.exec(range.headers.get('Content-Range')||'');
        if(!match||Number(match[1])!==offset)throw new Error('Invalid recording range');
        const data=new Uint8Array(range.data);total=Number(match[3]);
        if(data.length!==Number(match[2])-offset+1||!data.length)throw new Error('Incomplete recording range');
        offset+=data.length;
        for(const frame of parser.push(data))await append(frame);
      }
      for(const frame of parser.push(new Uint8Array(),true))await append(frame);
      if(!source)throw new Error('Recording is empty or unsupported');
      if(!signal.aborted&&media.readyState==='open')media.endOfStream();
      if(!started)video.play().catch(()=>{});
    }catch(e){if(!signal.aborted)status.textContent=e.message;}
  }
  window.pbsPreview={
    recording:(name,directory)=>previewRecording(name,directory),
    snapshot(blob){
      open('Snapshot');url=URL.createObjectURL(blob);const image=document.createElement('img');image.src=url;image.alt='Console snapshot';content.appendChild(image);
      download.href=url;download.download='pbs.png';status.textContent='Snapshot preview';
    }
  };
}
