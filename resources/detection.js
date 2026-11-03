/* Frame-clock metadata comes in the same FLV connection as its video frame. */
window.DetectionOverlay = (() => {
  let clocks=[], results=[], mode=0, video, canvas, context, started=false, active=false;
  let lastFrame=0,lastPoll=0,clockError=null;
  let transport='flv',mjpegCapture=0,latestAge=0,latestState='';
  const labels={person:'人',bottle:'瓶子',cup:'杯子',chair:'椅子',book:'书',laptop:'笔记本电脑',mouse:'鼠标','cell phone':'手机',bus:'公交车',car:'汽车'};
  function clear(){if(context){context.clearRect(0,0,canvas.width,canvas.height);
    delete canvas.dataset.frameCaptureUs;delete canvas.dataset.resultCaptureUs;
    delete canvas.dataset.videoPtsMs;delete canvas.dataset.clockPtsMs;}}
  function reset(){clocks=[];results=[];lastFrame=0;clockError=null;clear();}
  function attach(player,v){
    video=v;reset();active=true;transport='flv';
    player.on(flvjs.Events.SCRIPTDATA_ARRIVED,data=>{
      if(!data.onFrameClock)return;
      try{const item=typeof data.onFrameClock==='string'?JSON.parse(data.onFrameClock):data.onFrameClock;
        if(Number.isFinite(item.pts_ms)&&Number.isFinite(item.capture_us)){
          if(clocks.length && item.pts_ms<clocks[clocks.length-1].pts_ms)reset();
          clocks.push(item);if(clocks.length>600)clocks.shift();
        }
      }catch(e){clockError=e.message;}
    });
  }
  function draw(mediaTime){
    if(!canvas||!video)return;
    if(canvas.width!==canvas.clientWidth||canvas.height!==canvas.clientHeight){canvas.width=canvas.clientWidth;canvas.height=canvas.clientHeight;}
    clear();
    const info=document.getElementById('detectionInfo');
    if(!mode){if(info)info.textContent='检测已关闭 · 普通直播';return;}
    if(!active||(transport==='flv'&&video.style.display==='none')){if(info)info.textContent='等待当前协议的帧时钟';return;}
    const pts=mediaTime*1000;
    let frame=null;
    if(transport==='mjpeg')frame=mjpegCapture?{capture_us:mjpegCapture,pts_ms:0}:null;
    else for(const c of clocks){if(c.pts_ms<=pts+2)frame=c;else break;}
    if(!frame||(transport==='flv'&&Math.abs(frame.pts_ms-pts)>80)){if(info)info.textContent='等待当前视频帧的时间戳';return;}
    let match=null;
    const allowance=document.getElementById('detectHold').checked?250000:1;
    // Strict by default: only inference from exactly this camera frame.
    // Optional hold can use a past frame, never a future one.
    for(const r of results){if(r.mode===mode && r.frame_mono_us<=frame.capture_us+1 &&
      frame.capture_us-r.frame_mono_us<=allowance && (!match||r.frame_mono_us>match.frame_mono_us))match=r;}
    if(!match){if(info)info.textContent='本帧未推理或结果未就绪（不提前画框）';return;}
    const scale=Math.min(canvas.width/match.source_width,canvas.height/match.source_height);
    const ox=(canvas.width-match.source_width*scale)/2,oy=(canvas.height-match.source_height*scale)/2;
    context.strokeStyle='#42ff9a';context.fillStyle='#42ff9a';context.lineWidth=2;context.font='15px sans-serif';
    for(const b of match.boxes||[]){
      if(b.class_id===0 && !(mode&1)||b.class_id!==0 && !(mode&2))continue;
      context.strokeRect(ox+b.x*scale,oy+b.y*scale,b.width*scale,b.height*scale);
      context.fillText((labels[b.label]||b.label)+' '+Math.round(b.score*100)+'%',ox+b.x*scale,Math.max(18,oy+b.y*scale-4));
    }
    if(info)info.textContent=`${match.model} · ${Number(match.fps).toFixed(1)}fps · ${match.boxes.length}个目标 · 推理${Number(match.run_ms).toFixed(1)}ms · 框落后本帧${((frame.capture_us-match.frame_mono_us)/1000).toFixed(0)}ms`;
    canvas.dataset.frameCaptureUs=frame.capture_us;canvas.dataset.resultCaptureUs=match.frame_mono_us;
    canvas.dataset.videoPtsMs=pts;canvas.dataset.clockPtsMs=frame.pts_ms;
  }
  function frameCallback(now,meta){lastFrame=performance.now();draw(meta.mediaTime);video.requestVideoFrameCallback(frameCallback);}
  async function poll(){
    try{
      const [status,history]=await Promise.all([fetch('/api/status',{cache:'no-store'}).then(r=>r.json()),
        fetch('/api/detection/results',{cache:'no-store'}).then(r=>r.ok?r.json():{results:[]})]);
      mode=(status.inference||{}).mode||0;results=history.results||[];
      latestAge=Number(((status.inference||{}).result||{}).frame_age_ms||0);
      latestState=((status.inference||{}).result||{}).state||'';
      document.getElementById('detectPerson').checked=!!(mode&1);document.getElementById('detectObjects').checked=!!(mode&2);
      lastPoll=performance.now();
    }catch(e){results=[];}
    setTimeout(poll,200);
  }
  async function change(){
    const desired=(document.getElementById('detectPerson').checked?1:0)|(document.getElementById('detectObjects').checked?2:0);
    mode=desired;results=[];clear();
    const buttons=['off','person','objects','all'];
    try{const r=await fetch('/api/detection/'+buttons[desired],{method:'POST'});if(!r.ok||!(await r.json()).ok)throw Error('切换失败');}
    catch(e){document.getElementById('detectionInfo').textContent=e.message;}
  }
  function init(){
    if(started)return;started=true;
    canvas=document.getElementById('detectionCanvas');context=canvas.getContext('2d');video=document.getElementById('flvView');
    document.getElementById('detectPerson').onchange=change;document.getElementById('detectObjects').onchange=change;
    if(video.requestVideoFrameCallback)video.requestVideoFrameCallback(frameCallback);
    setInterval(()=>{
      if(performance.now()-lastPoll>1500||performance.now()-lastFrame>500)clear();
      if(!video.requestVideoFrameCallback)draw(video.currentTime);
    },100);
    poll();
  }
  document.addEventListener('DOMContentLoaded',init);
  return {attach,detach(){active=false;reset();},
    mjpegFrame(stamp){transport='mjpeg';active=true;mjpegCapture=stamp;lastFrame=performance.now();draw(0);},
    bufferDelay(){return mode?Math.min(900,Math.max(150,latestAge+250)):0;},
    debug(){return {mode,clocks:clocks.length,results:results.length,lastPoll,lastFrame,clockError};}};
})();
