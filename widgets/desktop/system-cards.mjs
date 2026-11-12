export const CLOCK_SIZES=[{value:72,label:'小 · 72'},{value:82,label:'中 · 82'},{value:92,label:'大 · 92'}];
const types=new Set(['system.clock','system.calendar','system.status','system.storage','system.todo']);
export const isSystemCard=type=>types.has(type);
export const clockSize=settings=>CLOCK_SIZES.some(size=>size.value===settings?.fontSize)?settings.fontSize:82;
const numeric=value=>typeof value==='number'&&Number.isFinite(value);
const percentage=value=>numeric(value)?Math.max(0,Math.min(100,value)):null;
const ns='http://www.w3.org/2000/svg';
// Picker illustrations only. Desktop instances cannot select this branch.
const previewSources={
 'system.cores':[{usage:12}], 'system.gpus':[{usage:13,name:'示例显卡'}],
 'system.memory':{total:31.5*1073741824,free:(31.5-12.7)*1073741824},
 'system.disks':[{mountPoint:'示例卷',isRemovable:false,totalSpace:1400*1073741824,availableSpace:(1400-1095)*1073741824}],
};

export function systemUsage(cores,gpus){
 const cpu=Array.isArray(cores)?cores.map(core=>percentage(core.usage)).filter(value=>value!==null):[];
 const gpu=Array.isArray(gpus)?gpus.filter(gpu=>percentage(gpu.usage)!==null).sort((a,b)=>b.usage-a.usage)[0]:null;
 return {cpu:cpu.length?cpu.reduce((sum,value)=>sum+value,0)/cpu.length:null,gpu:gpu?percentage(gpu.usage):null,gpuName:gpu?.name??''};
}
export function memoryUsage(memory){
 if(!memory||!numeric(memory.total)||memory.total<=0||!numeric(memory.free)||memory.free<0||memory.free>memory.total)return null;
 const used=memory.total-memory.free;return {total:memory.total,used,percent:used/memory.total*100};
}
export function diskUsage(disks){
 const seen=new Set(),drives=[];
 for(const disk of Array.isArray(disks)?disks:[]){
  const key=typeof disk.mountPoint==='string'?disk.mountPoint.replaceAll('/','\\').toLowerCase():'';
  if(!key||seen.has(key)||disk.isRemovable!==false||!numeric(disk.totalSpace)||disk.totalSpace<=0||!numeric(disk.availableSpace)||disk.availableSpace<0||disk.availableSpace>disk.totalSpace)continue;
  seen.add(key);drives.push(disk);
 }
 if(!drives.length)return null;
 const total=drives.reduce((sum,disk)=>sum+disk.totalSpace,0),used=drives.reduce((sum,disk)=>sum+disk.totalSpace-disk.availableSpace,0);
 return {total,used,percent:used/total*100,mounts:drives.map(disk=>disk.mountPoint)};
}

// Same built-in renderer for static picker snapshots and active desktop cards.
// Previews never subscribe, schedule updates, or collect hardware information.
export function renderSystemCard(root,context){
 const {type}=context.definition,document=root.ownerDocument,preview=context.preview===true;
 if(!isSystemCard(type))throw new Error('Unknown system card');
 root.classList.add('system-card-content');
 const node=(tag,cls,text,parent=root)=>{const element=document.createElement(tag);element.className=cls;if(text!==undefined)element.textContent=text;parent.append(element);return element;};
 const surface=node('div','system-card-surface');surface.dataset.kind=type.slice(7);
 const wide=type==='system.clock',width=wide?376:184;
 surface.style.width=width+'px';surface.style.height='184px';
 const fit=()=>{const scale=root.clientWidth&&root.clientHeight?Math.min(root.clientWidth/width,root.clientHeight/184):1;surface.style.transform=`scale(${Math.max(.01,scale)})`;};
 let observer=null;
 if(!preview&&globalThis.ResizeObserver){observer=new ResizeObserver(fit);observer.observe(root);}if(!preview)fit();
 const set=(element,value)=>{if(element.textContent!==value)element.textContent=value;};
 const dateValue=value=>{const date=value instanceof Date?value:new Date(value);return Number.isFinite(date.getTime())?date:new Date();};
 const snapshot=context.now??new Date();
 const source=(name,listener)=>{if(preview){if(previewSources[name])listener({available:true,data:previewSources[name]});}else context.subscribe(name,listener);};
 function svg(parent,paths,cls,viewBox='0 0 24 24'){
  const icon=document.createElementNS(ns,'svg');icon.setAttribute('viewBox',viewBox);icon.setAttribute('class',cls);icon.setAttribute('aria-hidden','true');
  for(const [tag,attributes] of paths){const child=document.createElementNS(ns,tag);for(const [key,value] of Object.entries(attributes))child.setAttribute(key,String(value));icon.append(child);}parent.append(icon);return icon;
 }
 const stroke={fill:'none',stroke:'currentColor','stroke-width':2,'stroke-linecap':'round','stroke-linejoin':'round'};
 const gauge=[['circle',{...stroke,cx:12,cy:12,r:9}],['path',{...stroke,d:'M12 12l3.5-3.5'}]];
 const thermometer=[['path',{...stroke,d:'M10 13V5a2 2 0 0 1 4 0v8'}],['circle',{...stroke,cx:12,cy:16.5,r:4}]];
 if(type==='system.clock'){
  surface.style.setProperty('--system-clock-size',clockSize(context.instance?.settings)+'px');
  const date=node('div','system-clock-date',undefined,surface),time=node('div','system-clock-time',undefined,surface);
  const paint=value=>{const now=dateValue(value);set(date,`${now.getMonth()+1}月${now.getDate()}日 星期${'日一二三四五六'[now.getDay()]}`);set(time,String(now.getHours()).padStart(2,'0')+':'+String(now.getMinutes()).padStart(2,'0'));};
  paint(snapshot);source('system.minute',paint);
 }else if(type==='system.calendar'){
  const heading=node('div','system-calendar-heading',undefined,surface),grid=node('div','system-calendar-grid',undefined,surface);
  let monthKey='';
  const paint=value=>{
   const now=dateValue(value),key=now.getFullYear()+'-'+now.getMonth()+'-'+now.getDate();if(key===monthKey)return;monthKey=key;
   set(heading,(now.getMonth()+1)+'月');heading.title=now.getFullYear()+'年'+(now.getMonth()+1)+'月';grid.replaceChildren();
   for(const day of '一二三四五六日')node('span','system-calendar-weekday',day,grid);
   const lead=(new Date(now.getFullYear(),now.getMonth(),1).getDay()+6)%7,days=new Date(now.getFullYear(),now.getMonth()+1,0).getDate(),cells=Math.ceil((lead+days)/7)*7;
   for(let i=0;i<cells;i++){const day=i-lead+1,cell=node('span','system-calendar-day',day>0&&day<=days?String(day):'',grid);if(day===now.getDate()){cell.classList.add('is-today');cell.setAttribute('aria-current','date');}}
   grid.style.gridTemplateRows=`repeat(${cells/7+1},1fr)`;
  };paint(snapshot);source('system.day',paint);
 }else if(type==='system.status'){
  const row=(label,icon)=>{const r=node('div','system-status-row',undefined,surface);svg(r,icon,'system-status-icon');node('span','system-status-label',label,r);return node('span','system-status-value','—',r);};
  const section=label=>{node('div','system-status-title',label,surface);node('div','system-card-spacer',undefined,surface);const usage=row('使用率',gauge),temperature=row('温度',thermometer);temperature.title='当前系统接口未提供可靠的温度数据';node('div','system-card-spacer',undefined,surface);return usage;};
  const cpu=section('CPU');node('div','system-card-divider',undefined,surface);const gpu=section('GPU');
  const fmt=value=>value===null?'—':Math.round(value)+'%';
  source('system.cores',value=>set(cpu,fmt(systemUsage(value.available?value.data:null,null).cpu)));
  source('system.gpus',value=>{const data=systemUsage(null,value.available?value.data:null);set(gpu,fmt(data.gpu));gpu.title=data.gpuName?data.gpuName+'（多显卡时显示使用率最高的一张）':'未取得显卡数据';});
 }else{
  const block=label=>{
   const group=node('div','system-storage-block',undefined,surface),body=node('div','system-storage-body',undefined,group);
   node('div','system-storage-title',label,body);const value=node('div','system-storage-value','—',body),ring=node('div','system-storage-ring',undefined,group);
   const icon=svg(ring,[['circle',{cx:17,cy:17,r:14,fill:'none',stroke:'currentColor','stroke-opacity':.2,'stroke-width':2.6}],['circle',{cx:17,cy:17,r:14,fill:'none',stroke:'currentColor','stroke-width':2.6,'stroke-linecap':'round','stroke-dasharray':87.9646,'stroke-dashoffset':87.9646}]],'system-ring-svg','0 0 34 34');
   const percent=node('span','system-storage-percent','—',ring),arc=icon.lastChild;
   if(label==='磁盘'&&context.honorServices===true){
    const clean=node('button','system-storage-cleanup','打开管家清理',body);clean.type='button';clean.disabled=preview;clean.title='打开电脑管家清理页并扫描，实际清理由你在管家中确认';
    clean.addEventListener('click',async()=>{clean.disabled=true;try{await context.openStorageCleanup();}catch{}finally{if(!context.signal?.aborted)clean.disabled=false;}});
   }
   return data=>{
    if(!data){set(value,'—');set(percent,'—');arc.setAttribute('stroke-dashoffset','87.9646');group.classList.remove('is-warning');return;}
    const gib=bytes=>{const number=bytes/1073741824;return number.toFixed(number>=100?0:1);};
    value.replaceChildren(document.createTextNode(gib(data.used)));node('span','system-storage-total','/'+gib(data.total),value);value.append(document.createTextNode(' GiB'));
    set(percent,Math.round(data.percent)+'%');arc.setAttribute('stroke-dashoffset',String(87.9646*(1-data.percent/100)));group.classList.toggle('is-warning',label==='磁盘'&&data.percent>=75);
    group.title=(data.mounts?.join('、')??'物理内存')+' · 已使用 / 总容量';
   };
  };
  const memory=block('RAM');node('div','system-card-divider',undefined,surface);const disk=block('磁盘');
  source('system.memory',value=>memory(memoryUsage(value.available?value.data:null)));
  source('system.disks',value=>disk(diskUsage(value.available?value.data:null)));
 }
 return ()=>{observer?.disconnect();root.classList.remove('system-card-content');};
}
