// Read multipart headers ourselves: a native <img> cannot expose each part's timestamp.
window.MjpegView=(()=>{
 let abort=null,queue=[],canvas,ctx,onFrame,onError,timer=null,generation=0;
 function stop(){generation++;if(abort)abort.abort();abort=null;if(timer)clearTimeout(timer);timer=null;
   for(const f of queue)f.bitmap.close();queue=[];}
 function render(){
  const now=performance.now(),delay=window.DetectionOverlay.bufferDelay();let chosen=null;
  while(queue.length && (queue[0].received+delay<=now||queue.length>20)){
    if(chosen)chosen.bitmap.close();chosen=queue.shift();
  }
  if(chosen){canvas.width=canvas.clientWidth;canvas.height=canvas.clientHeight;
    ctx.fillStyle='#000';ctx.fillRect(0,0,canvas.width,canvas.height);
    const b=chosen.bitmap,s=Math.min(canvas.width/b.width,canvas.height/b.height);
    ctx.drawImage(b,(canvas.width-b.width*s)/2,(canvas.height-b.height*s)/2,b.width*s,b.height*s);
    onFrame(chosen.capture);b.close();}
  timer=setTimeout(render,16);
 }
 async function start(target,frame,error){
  stop();const token=generation;canvas=target;ctx=canvas.getContext('2d');onFrame=frame;onError=error;
  abort=new AbortController();render();
  let bytes=new Uint8Array(),part=null;
  try{
   const response=await fetch('/mjpeg',{signal:abort.signal,cache:'no-store'});
   if(!response.ok)throw Error('MJPEG HTTP '+response.status);
   const reader=response.body.getReader();
   while(token===generation){
    const {value,done}=await reader.read();if(done)throw Error('MJPEG stream ended');
    const merged=new Uint8Array(bytes.length+value.length);merged.set(bytes);merged.set(value,bytes.length);bytes=merged;
    if(bytes.length>4*1024*1024)throw Error('MJPEG buffer limit exceeded');
    while(token===generation){
     if(!part){let end=-1;for(let i=0;i+3<bytes.length;i++)if(bytes[i]===13&&bytes[i+1]===10&&bytes[i+2]===13&&bytes[i+3]===10){end=i;break;}
      if(end<0)break;
      const header=new TextDecoder().decode(bytes.subarray(0,end));
      const length=/Content-Length:\s*(\d+)/i.exec(header),stamp=/X-Capture-Mono-Us:\s*(\d+)/i.exec(header);
      if(!length)throw Error('MJPEG part missing Content-Length');
      part={length:Number(length[1]),capture:stamp?Number(stamp[1]):0};
      if(part.length<4||part.length>2*1024*1024)throw Error('MJPEG invalid part size');
      bytes=bytes.slice(end+4);
     }
     if(bytes.length<part.length+2)break;
     const jpeg=bytes.slice(0,part.length),capture=part.capture;bytes=bytes.slice(part.length+2);part=null;
     const bitmap=await createImageBitmap(new Blob([jpeg],{type:'image/jpeg'}));
     if(token!==generation){bitmap.close();break;}
     queue.push({bitmap,capture,received:performance.now()});
     while(queue.length>24)queue.shift().bitmap.close();
    }
   }
  }catch(e){if(token===generation && e.name!=='AbortError'){stop();onError(e);}}
 }
 return {start,stop};
})();
