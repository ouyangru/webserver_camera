// Execute the real overlay code with a deterministic video clock and network.
const fs=require('fs'),vm=require('vm'),assert=require('assert');
let nextPoll,frameCallback,scriptCallback,draws=0;
let mode=1,results=[];
const canvas={width:640,height:480,clientWidth:640,clientHeight:480,dataset:{},
 getContext:()=>({clearRect(){draws=0;},strokeRect(){draws++;},fillText(){}})};
const video={style:{display:'block'},requestVideoFrameCallback(fn){frameCallback=fn;}};
const elements={detectionCanvas:canvas,flvView:video,detectPerson:{},detectObjects:{},detectHold:{checked:false},detectionInfo:{}};
let init;
const sandbox={window:{},document:{getElementById:id=>elements[id],addEventListener:(name,fn)=>{init=fn;}},
 performance:{now:()=>100},flvjs:{Events:{SCRIPTDATA_ARRIVED:'script'}},
 setTimeout(fn){nextPoll=fn;},setInterval(){},currentTab:'flv',
 fetch:async url=>({ok:true,json:async()=>url==='/api/status'?{inference:{mode}}:{results}})};
vm.runInNewContext(fs.readFileSync(require('path').join(__dirname,'../resources/detection.js'),'utf8'),sandbox);
sandbox.window.DetectionOverlay.attach({on(event,fn){scriptCallback=fn;}},video);
init();
const flush=()=>new Promise(resolve=>setImmediate(resolve));
(async()=>{
 await flush();
 scriptCallback({onFrameClock:JSON.stringify({capture_us:1000000,pts_ms:0})});
 async function check(stamp,expected,hold=false){
  results=[{mode:1,frame_mono_us:stamp,source_width:640,source_height:480,model:'test',fps:20,run_ms:12,
    boxes:[{label:'person',class_id:0,score:.9,x:1,y:2,width:30,height:40}]}];
  elements.detectHold.checked=hold;await nextPoll();frameCallback(100,{mediaTime:0});
  assert.strictEqual(draws,expected,`stamp=${stamp} mode=${mode} hold=${hold}`);
  if(expected)assert(Number(canvas.dataset.resultCaptureUs)<=Number(canvas.dataset.frameCaptureUs));
  else assert.strictEqual(canvas.dataset.resultCaptureUs,undefined);
 }
 await check(1000100,0); // Future result must never be drawn.
 await check(1000000,1); // Exactly the displayed frame.
 await check(990000,0);  // Past frame is not exact.
 await check(990000,1,true); // Explicit optional past-frame hold.
 await check(700000,0,true); // Past frame expired.
 mode=0;await check(1000000,0);
 console.log('overlay future/exact/past/expiry/off tests passed');
})().catch(error=>{console.error(error);process.exitCode=1;});
