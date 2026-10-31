// Local browser regression against tests/http/dashboard_fixture.c.

const assert=require('node:assert/strict'),path=require('node:path'),fs=require('node:fs');
const root=path.resolve(__dirname,'../..'),out=path.join(root,'build/issue-tests/browser');fs.mkdirSync(out,{recursive:true});
const {chromium}=require(process.env.PLAYWRIGHT_MODULE||path.join(root,'build/web-player-test/node_modules/playwright'));
const endpoint=process.env.PBS_TEST_URL||'http://127.0.0.1:18101';
assert.ok(['127.0.0.1','localhost'].includes(new URL(endpoint).hostname),'browser regression only runs against a loopback fixture');
(async()=>{
 const browser=await chromium.launch({...(process.env.PBS_CHROME?{executablePath:process.env.PBS_CHROME}:{channel:'chrome'}),headless:true});
 try{
  for(const mobile of [false,true]){
   const page=await browser.newPage({viewport:mobile?{width:390,height:844}:{width:1440,height:1000},isMobile:mobile,hasTouch:mobile});const errors=[];page.on('pageerror',e=>errors.push(e.message));
   await page.goto(endpoint);
   await page.waitForFunction(()=>document.getElementById('video').currentTime>0.5&&window.pbsPreview);
   const settings=await (await page.request.get(endpoint+'/recording/settings')).json();
   const base=settings.directory.split(/\/browser-\d+/)[0];
   assert.ok(base.includes('/build/issue-tests/'),'recordings must remain inside the local fixture directory');
   await page.locator('summary').filter({hasText:'Recording storage'}).click();
   const dir=base+'/browser-'+Date.now();
   await page.locator('#record-directory').fill(dir);await page.locator('#save-directory').click();
   await page.waitForFunction(p=>document.getElementById('storage-info').textContent.includes(p),dir);
   const options=await page.locator('#resolution option').allTextContents();assert.ok(options.some(v=>v.includes('2160p')));
   assert.equal(await page.locator('#resolution').evaluate(e=>Boolean(e.closest('live-video-skin'))),true,'resolution belongs to the player');
   await page.locator('#resolution').selectOption('2160');await page.locator('#apply-resolution').click();
   await page.waitForFunction(()=>document.getElementById('profile-info').textContent.includes('2160p could not start'));
   await page.locator('#snapshot').click();await page.waitForFunction(()=>document.getElementById('preview-dialog').open&&document.querySelector('#preview-content img')?.naturalWidth>0);
   assert.equal(await page.locator('#preview-download').getAttribute('download'),'pbs.png');
   await page.locator('#close-preview').click();
   await page.locator('#record').click();await page.waitForFunction(()=>recording);
   assert.equal(await page.locator('#save-directory').isDisabled(),true);
   await page.waitForTimeout(2500);await page.locator('#record').click();await page.waitForFunction(()=>!recording);
   await page.locator('.recording-card button').filter({hasText:'Preview'}).first().click();
   await page.waitForFunction(()=>document.querySelector('#preview-content video')?.currentTime>0.4,{}, {timeout:15000});
   assert.equal(await page.evaluate(()=>document.querySelector('#preview-content video').videoWidth),1280);
   assert.equal(await page.evaluate(()=>document.querySelector('#preview-content pbs-preview-player').store.state.mutedAvailability),'unavailable');
   assert.equal(await page.evaluate(()=>viewerPaused),true,'live video pauses while previewing a recording');
   await page.screenshot({path:path.join(out,mobile?'issues-preview-mobile.png':'issues-preview-desktop.png'),fullPage:true});
   await page.locator('#close-preview').click();await page.waitForFunction(()=>!viewerPaused&&!document.getElementById('video').paused);
   assert.ok(await page.evaluate(()=>document.documentElement.scrollWidth<=innerWidth),'no horizontal overflow');
   await page.screenshot({path:path.join(out,mobile?'issues-mobile.png':'issues-desktop.png'),fullPage:true});
   await page.evaluate(()=>{
    window.confirm=()=>{throw new Error('Blocking confirmation must not interrupt live PCM');};
    window.pbsAudio.set({muted:false});
   });
   await page.waitForFunction(()=>audioOn&&audioCtx.state==='running');
   await page.evaluate(()=>{
    window.audioCallbacks=0;const process=audioSource.onaudioprocess;
    audioSource.onaudioprocess=e=>{window.audioCallbacks++;process(e);};
   });
   for(const cancel of [true,false]){
    await page.locator('.recording-card button').filter({hasText:'Delete'}).first().click();
    await page.waitForFunction(()=>document.getElementById('delete-dialog').open);
    const before=await page.evaluate(()=>({audio:window.audioCallbacks,video:document.getElementById('video').currentTime}));
    await page.waitForTimeout(700);
    const after=await page.evaluate(()=>({audio:window.audioCallbacks,video:document.getElementById('video').currentTime}));
    assert.ok(after.audio>before.audio,'PCM processing continues while confirmation is open');
    assert.ok(after.video>before.video,'live video continues while confirmation is open');
    await page.locator('#delete-dialog button').filter({hasText:cancel?'Cancel':'Delete recording'}).click();
    await page.waitForFunction(()=>!document.getElementById('delete-dialog').open);
    if(cancel)assert.equal(await page.locator('.recording-card').count(),1,'cancel preserves the recording');
   }
   await page.waitForFunction(()=>document.getElementById('recordings').textContent==='No recordings yet');
   assert.deepEqual(errors,[]);
   assert.equal(await page.evaluate(()=>audioOn&&audioCtx.state==='running'),true,'deleting preserves live audio');
   console.log('PASS production HTTP + Chrome',mobile?'mobile':'desktop','resolution controls/rollback, storage selection, recording, snapshot, bounded H264 preview, restore live, deletion/cancel with continuous PCM');
   await page.close();
  }
 }finally{await browser.close();}
})().catch(e=>{console.error(e);process.exitCode=1;});
