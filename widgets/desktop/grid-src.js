import {windowsDesktopMenu} from "./windows-desktop-menu.mjs";
import {creationPosition} from "./creation-placement.mjs";
import {desktopMenuBounds} from "./desktop-menu-bounds.mjs";
import {fileDropTarget} from "./file-drop-target.mjs";
import {folderCover} from "./folder-cover.mjs";
import {menuChannel} from "./menu-channel.mjs";
import {crossPreviewChannel,remotePlacement} from "./cross-display-preview.mjs";
import {frame as gridFrame,position as gridPosition,placementError} from "./unified-geometry.mjs";
import { Widget, ConnectedMonitorList, Settings } from "@seelen-ui/lib";
import { getCurrentWebview } from "@tauri-apps/api/webview";
import {invoke} from "@tauri-apps/api/core";
import { folderMotion } from "./folder-motion.mjs";
import { createFolderContents } from "./folder-contents.mjs";
import { installExternalDrop } from "./external-drop.mjs";
import {desktopKeyboard} from './desktop-keyboard.mjs';
import {outboundDrag} from './outbound-drag.mjs';
import {dragMembers,groupOffsets,groupPlacement,nearestGroupPlacement} from './group-drag.mjs';
import {selectionState,installMarquee} from './desktop-selection.mjs';
import {createDesktopCardHost} from './card-host.mjs';
import {createDesktopCardClient} from './card-client.mjs';
import {desktopCardRenderers,createCardDataHub} from './card-registry.mjs';
import './card-modules.mjs';
import {renderDeclarativeCard} from './declarative-cards.mjs';
import {createCardManager} from './card-manager.mjs';
import {CLOCK_SIZES,clockSize} from './system-cards.mjs';
import {todoCardMenuState,runTodoCardMenuAction} from './todo-card.mjs';
import {createDesktopPages} from './desktop-pages.mjs';
import {desktopPageMotion} from './desktop-page-motion.mjs';

const session = globalThis.__MAGIC_BRIDGE, widget = Widget.self, monitorId = widget.decoded.monitorId;
const frontendVersion=typeof __MAGIC_DESKTOP_VERSION__==='undefined'?'development-source':__MAGIC_DESKTOP_VERSION__;
const grid = document.getElementById("desktop-grid"), viewport = document.getElementById("grid-viewport"), dialog = document.getElementById("folder-dialog"), status = document.getElementById("status"), menu = document.getElementById("folder-menu");
const pageMotion=desktopPageMotion(grid);
const motion = folderMotion(dialog), sizes = [[2, 2], [4, 2], [4, 4]];
let model, board, byId = new Map(), selected = null, selectedTile = null, openFolderId = null, menuFolder = null;
let gesture = null, ghost = null, scale = 1, suppressClicksUntil = 0, busy = false, updating = null, pendingUpdate = false, stopped = false, changeVersion = 0;
let watchController = null,externalDrop=null,menus=null,windowsMenu=null,keyboard=null,outbound=null,monitorUnsubscribe=null,settingsUnsubscribe=null,desktopSettings=null;
const selection=selectionState();let marquee=null,suppressMarqueeClick=false;
const selectionScope=sourceFolder=>sourceFolder?'folder:'+sourceFolder:'desktop';
const folderContents=createFolderContents({dialog,host:document.getElementById('folder-contents'),item:id=>byId.get(id),create:(id,folderId)=>shortcut(id,folderId),focused:(id,folderId)=>select(id,folderId),
 patch:(value,item,folderId)=>{if(value.title!==item.name){value.title=item.name;value.setAttribute('aria-label',item.name);value.querySelector('.app-name').textContent=item.name;}updateIcon(value.querySelector('.icon'));const selected=selection.has(item.id,'folder:'+folderId);value.classList.toggle('selected',selected);value.setAttribute('aria-selected',String(selected));},
 blocked:()=>!!gesture||!!windowsMenu?.renaming});
const folderKey = id => "folder:" + id, itemKey = id => "item:" + id;
const tileKey=value=>value.kind==='folder'?folderKey(value.folderId):value.kind==='card'?'card:'+value.cardId:itemKey(value.itemId);
const cardData=createCardDataHub(),cardClient=createDesktopCardClient({request,monitorId,revision:()=>model.revision,changed:()=>update(true)});
async function cardAction(method,...values){if(busy||model.readOnly)throw new Error('上一项操作尚未完成或布局只读。');busy=true;try{return await cardClient[method](...values);}finally{busy=false;}}
const cardManager=createCardManager({document,client:cardClient,getProjection:()=>model?.projection,create:(type,columns,rows,settings={})=>cardAction('create',type,columns,rows,settings),focus:anchor=>keyboard?.take(anchor)??Promise.resolve(false),changed:()=>update(true),onError:error=>message(error.message,true)});
const cardHost=createDesktopCardHost({document,renderers:{get:(type,definition)=>definition?.custom?renderDeclarativeCard:desktopCardRenderers.get(type)},dataHub:cardData,configure:(id,settings)=>cardAction('configure',id,settings),openStorageCleanup:async id=>{await request('/desktop-card-cleanup',{monitorId,id});message('已请求打开电脑管家清理页；请在管家中确认清理。');},shortcut:(id,index)=>request('/desktop-card-shortcut',{monitorId,id,index}),focus:anchor=>keyboard?.take(anchor)??Promise.resolve(false),onError:error=>message(error.message,true),
 onDragStart:(event,value,source)=>{if(event.button!==0||model.readOnly||busy)return;clearGesture();hideMenu();clearSelection();selectedTile='card:'+value.id;const rect=source.getBoundingClientRect();gesture={kind:'card',cardId:value.id,pointerId:event.pointerId,x:event.clientX,y:event.clientY,offset:grip(event,rect),source,active:false};},
 // The Seelen host cancels the browser menu in capture phase. That does not
 // consume the app's context menu, just as for existing icons and folders.
 onContextMenu:(event,value,source)=>{if(value.type!=='system.todo'&&event.target.closest('input,textarea,select,[contenteditable=true]'))return;event.preventDefault();event.stopPropagation();const rect=source.getBoundingClientRect();showCardMenu(value,event.clientX??rect.left+12,event.clientY??rect.top+12,source);}});
const cell = () => model?.projection.stepX || 96;
const grip=(event,rect)=>({x:Math.max(0,Math.min(rect.width,event.clientX-rect.left)),y:Math.max(0,Math.min(rect.height,event.clientY-rect.top))});
let preview=null,dragFrame=0,dragPoint=null,dropTarget=null,pageChangePromise=null;
const pages=createDesktopPages({document,window,getModel:()=>model,getDrag:()=>gesture,
 blocked:()=>busy||!model||model.readOnly||dialog.open||cardManager.isOpen||!!windowsMenu?.renaming||!!document.querySelector('.windows-desktop-menu:not([hidden]),.folder-menu:not([hidden]):not(.desktop-page-menu)')||!!marquee?.active,
 navigate:async(action,dragging)=>{
  if(busy||!model||model.readOnly)return false;
  if(!dragging){if(gesture)clearGesture();clearSelection();hideMenu();}else hideMenu();
  busy=true;
  const currentIndex=board.pages?.findIndex(page=>page.id===board.pageId)??0,targetIndex=board.pages?.findIndex(page=>page.id===action.pageId)??currentIndex;
  const direction=action.kind==='add'?(action.before?-1:1):action.kind==='select'?Math.sign(targetIndex-currentIndex):0;
  pageChangePromise=pageMotion.run(direction,async()=>{try{await request('/desktop-page-action',{monitorId,revision:model.revision,...action});await update(true,dragging);return true;}catch(error){clearGesture();message(error.message,true);await update(true);return false;}finally{busy=false;}},{dragging});
  try{return await pageChangePromise;}finally{pageChangePromise=null;}
 },restore:tileId=>mutate('restore-unplaced',{tileId}),moveScreen:(tileId,boardId)=>mutate('move-screen',{tileId,boardId}),claim:claimMenu,focus:anchor=>keyboard?.take(anchor)});
let crossLast=null,crossHeartbeat=null;
function publishCross(data){crossLast=data.type==="preview"?data:null;clearTimeout(crossHeartbeat);crossChannel?.publish(data);if(crossLast)crossHeartbeat=setTimeout(()=>{if(gesture?.active&&crossPublished)publishCross(crossLast);},1000);}
let nativeFrame=null,crossChannel=null,crossPublished=false,remotePreview=null,remoteFrame=0,remoteData=null,remoteTimer=null,remoteSignature="";
function clearRemotePreview(){remoteData=null;remotePreview?.remove();remotePreview=null;remoteSignature="";clearTimeout(remoteTimer);if(remoteFrame)cancelAnimationFrame(remoteFrame);remoteFrame=0;}
function receiveCrossPreview(data){
  if(data.type==="clear"){if(remoteData?.source===data.source)clearRemotePreview();return;}
  remoteData=data;clearTimeout(remoteTimer);remoteTimer=setTimeout(clearRemotePreview,1500);
  if(!remoteFrame)remoteFrame=requestAnimationFrame(()=>{
    remoteFrame=0;if(!nativeFrame||!model||gesture?.active)return;
    const localX=(remoteData.point?.x-nativeFrame.rect.left)/nativeFrame.scale,localY=(remoteData.point?.y-nativeFrame.rect.top)/nativeFrame.scale;
    const candidate=remoteData.kind==='item'&&localX>=0&&localY>=0&&localX<innerWidth&&localY<innerHeight?destinationAt(localX,localY):null;
    const physical=candidate&&!remoteData.itemIds?.includes(candidate.item?.id)&&fileDropTarget(remoteData.itemId,candidate.item);
    let placed=physical?{rect:board.placements[itemKey(candidate.item.id)],valid:true}:remotePlacement(remoteData,nativeFrame,model.projection,{x:viewport.scrollLeft,y:viewport.scrollTop},board);
    if(!placed){clearRemotePreview();return;}if(!physical&&!placed.folderId&&remoteData.offsets?.length>1){const group=groupPlacement(placed.rect,remoteData.offsets,board);placed={rect:group.rect,valid:!group.error};}
    const signature=JSON.stringify(placed);if(signature===remoteSignature)return;remoteSignature=signature;
    const f=gridFrame(placed.rect,model.projection);
    if(!remotePreview){remotePreview=node("div","grid-drop-preview remote-drop-preview");document.body.append(remotePreview);}
    remotePreview.dataset.invalid=String(!placed.valid);remotePreview.style.width=f.width+"px";remotePreview.style.height=f.height+"px";remotePreview.style.transform=`translate(${f.left-viewport.scrollLeft}px,${f.top-viewport.scrollTop}px)`;
  });
}
let nativeReady=false,regionSignature="",regionPending=false,regionSending=false,toastTimer=null,regionFrame=0;
const newFolderControl=document.getElementById("new-folder"),settingsControl=document.getElementById("settings");
newFolderControl.textContent="＋";newFolderControl.title="新建大文件夹，右键查看更多操作";newFolderControl.setAttribute("aria-label","新建桌面大文件夹");
settingsControl.textContent="⋯";settingsControl.title="整理与调试";settingsControl.setAttribute("aria-label","打开整理与调试");
status.classList.add("accessory-toast");status.hidden=true;document.body.append(status);
let desktopMenuPoint=null;
const desktopMenu=node("div","folder-menu");desktopMenu.id="desktop-menu";desktopMenu.hidden=true;desktopMenu.setAttribute("role","menu");document.body.append(desktopMenu);
for(const [label,kind] of [["新建大文件夹","new-folder"],["校准网格布局","reposition-folders"],["记录问题","feedback"],["退出桌面","exit"]]){
  const button=node("button",null,label);button.addEventListener("click",()=>{const point=desktopMenuPoint;hideMenu();if(kind==="new-folder")void newFolderAt(point);else if(kind==="reposition-folders")void mutate(kind);else if(kind!=="exit"||confirm("退出 Magic 桌面配套？原有桌面和文件会保留。"))void basic(kind);});desktopMenu.append(button);
}
newFolderControl.addEventListener("contextmenu",event=>{event.preventDefault();claimMenu();desktopMenuPoint={x:event.clientX,y:event.clientY};desktopMenu.hidden=false;desktopMenu.style.left=Math.max(8,Math.min(event.clientX,innerWidth-desktopMenu.offsetWidth-8))+"px";desktopMenu.style.top=Math.max(8,Math.min(event.clientY,innerHeight-desktopMenu.offsetHeight-8))+"px";queueRegions();});
status.addEventListener("click",()=>{status.hidden=true;queueRegions();});
function node(tag, className, text) { const value = document.createElement(tag); if (className) value.className = className; if (text !== undefined) value.textContent = text; return value; }
function message(text, error = false) { status.textContent = text; status.classList.toggle("error", error);status.hidden=!error;clearTimeout(toastTimer);if(error)toastTimer=setTimeout(()=>{status.hidden=true;queueRegions();},6000);queueRegions(); }
function regionBody(){
  if(model?.mode==="unified-desktop")return {monitorId,viewport:{width:innerWidth,height:innerHeight},scrollX:viewport.scrollLeft,scrollY:viewport.scrollTop,regions:[{left:0,top:0,right:innerWidth,bottom:innerHeight}]};
  const regions=[];
  if(dialog.open)regions.push({left:0,top:0,right:innerWidth,bottom:innerHeight});
  else for(const element of [...grid.children,menu,desktopMenu,status,...(ghost?[ghost]:[])]){
    if(element.hidden||getComputedStyle(element).display==="none")continue;const rect=element.getBoundingClientRect();const left=Math.max(0,rect.left-2),top=Math.max(0,rect.top-2),right=Math.min(innerWidth,rect.right+2),bottom=Math.min(innerHeight,rect.bottom+2);if(right>left&&bottom>top)regions.push({left,top,right,bottom});
  }
  return {monitorId,viewport:{width:innerWidth,height:innerHeight},regions};
}
function queueRegions(){
  if(stopped)return;
  regionPending=true;if(!nativeReady||regionSending)return;
  if(gesture?.active){if(!regionFrame)regionFrame=requestAnimationFrame(()=>{regionFrame=0;sendRegions();});return;}
  if(regionFrame){cancelAnimationFrame(regionFrame);regionFrame=0;}sendRegions();
}
function sendRegions(){
  queueMicrotask(async()=>{if(stopped||regionSending||!nativeReady)return;regionSending=true;try{while(!stopped&&regionPending){regionPending=false;const body=regionBody(),signature=JSON.stringify(body);if(signature!==regionSignature){await request("/regions",body);regionSignature=signature;}}}catch(error){if(!stopped){console.error("MAGIC_FOLDER_REGION_FAILED",error.message);status.textContent="文件夹输入区域未更新，请从固定入口重进。";status.hidden=false;}}finally{regionSending=false;}});
}
async function request(path, body, timeout = 8000) {
  const response = await fetch(session.endpoint + path, { method: body === undefined ? "GET" : "POST", headers: { Authorization: "Bearer " + session.token, ...(body === undefined ? {} : { "Content-Type": "application/json" }) }, body: body === undefined ? undefined : JSON.stringify(body), ...(timeout>0?{signal:AbortSignal.timeout(timeout)}:{}) });
  const result = await response.json();
  if (!response.ok) { const error = new Error(result.error || "操作未完成。"); error.code=result.code;error.status=response.status;throw error; } return result;
}
async function mutate(kind, values = {}, text = "桌面布局已保存。") {
  if (busy) { message("正在保存上一项操作，请稍候。"); return false; }
  if (model.readOnly && kind !== "open") { message("布局当前为只读，原文件已保护。", true); return false; }
  busy = true;
  const moving=false;
  if(moving)message("正在移动并保存，请稍候…");
  try { const result=await request("/grid-action", { monitorId, kind, revision: model.revision, ...values },kind==='cross-drop'?0:8000); if (kind !== "open") await update(true); message(result.physicalOperation==='recycle'?'已移入 Windows 回收站。':result.physicalOperation==='move'?'已移动到真实文件夹。':text); return true; }
  catch (error) { message(error.message, true); try { await update(true); } catch {} return false; }
  finally { busy = false; }
}
async function basic(kind) {
  const started=kind==='refresh'?performance.now():null;
  try { await request("/action", { monitorId, kind }); if (kind === "refresh") { await update(true); message("桌面项目已刷新。"); console.info('MAGIC_DESKTOP_REFRESH',JSON.stringify({phase:'complete',elapsedMs:performance.now()-started,frontendVersion})); } }
  catch (error) { message(error.message, true); }
}
async function fileOperation(action,itemId,values={}){
  if(busy||model.readOnly){message("上一项操作未完成或布局只读，请稍后重试。",true);return;}
  busy=true;
  try{if(action==='rename')await update();await request('/desktop-file-operation',{monitorId,revision:model.revision,itemId,action,...values},0);await update(true);message(action==='recycle'?'已移入 Windows 回收站。':action==='rename'?'已重命名。':'已移动到真实文件夹。');}
  catch(error){message(error.message,true);await update(true);}
  finally{busy=false;}
}
async function groupFileOperation(itemIds,targetId,control=false,shift=false){
 if(busy||model.readOnly){message('上一项操作未完成或桌面只读，请稍后重试。',true);return;}busy=true;
 try{await request('/desktop-group-drop',{monitorId,revision:model.revision,itemIds,targetId,control,shift},0);await update(true);}
 catch(error){message(error.message,true);await update(true);}finally{busy=false;}
}
function creationValues(point,w=1,h=1){return creationPosition(point,model.projection,board,innerWidth,innerHeight,viewport.scrollLeft,viewport.scrollTop,w,h);}
function newFolderAt(point){return mutate("new-folder",creationValues(point,2,2));}
async function createDesktopItem(action,point){
  if(busy||model.readOnly){message("上一项操作未完成或布局只读，请稍后重试。",true);return;}
  busy=true;
  try{await request('/desktop-create-item',{monitorId,action,...creationValues(point)},0);await update(true);message(action==='folder'?'已新建 Windows 文件夹。':'已新建文本文档。');}
  catch(error){message(error.message,true);await update(true);}
  finally{busy=false;}
}
function icon(id, mini = false) {
  const value = node("span", mini ? "mini-icon" : "icon"); value.dataset.iconKey = id; updateIcon(value); return value;
}
function updateIcon(value) {
  const item = byId.get(value.dataset.iconKey); if (!item) return;
  if (item.icon) {
    if (value.dataset.image !== item.icon) { const image = node("img"); image.src = item.icon; image.alt = ""; image.draggable = false; value.replaceChildren(image); value.dataset.image = item.icon; value.classList.remove("fallback"); }
  } else if (!value.firstChild) { value.textContent = item.isFolder ? "▰" : item.name.slice(0, 1); value.classList.add("fallback"); }
}
function paintSelection(){
  selected=selection.primary;selectedTile=selection.size===1?(selection.scope==='desktop'?itemKey(selected):selection.scope):null;
  for(const value of document.querySelectorAll('[data-item]')){
    const source=value.closest('[data-folder]')?.dataset.folder||(value.closest('#folder-contents')?openFolderId:null);
    const yes=selection.has(value.dataset.item,selectionScope(source));value.classList.toggle('selected',yes);value.setAttribute('aria-selected',String(yes));
  }
  for(const value of grid.querySelectorAll('[data-folder]'))value.classList.toggle('folder-selected',selection.size===1&&value.dataset.tile===selectedTile);
  document.getElementById('add-selected').disabled=model.readOnly||selection.size!==1||!board.rootIds.includes(selected);
  document.getElementById('move-screen').disabled=model.readOnly||!selectedTile||document.getElementById('screen-target').options.length===0;
  document.getElementById('move-out').disabled=model.readOnly||selection.size!==1||!board.folders.find(folder=>folder.id===openFolderId)?.itemIds.includes(selected);
}
function select(id, sourceFolder=null, additive=false, context=false){
  if(context)selection.context(id,selectionScope(sourceFolder));else selection.click(id,selectionScope(sourceFolder),additive);paintSelection();
}
function selectedItems(){return selection.ids.map(id=>byId.get(id)).filter(Boolean);}
function clearSelection(){
  selection.clear();selected=null;selectedTile=null;keyboard?.cancel();
  for(const value of document.querySelectorAll('.shortcut.selected,.large-folder.folder-selected')){value.classList.remove('selected','folder-selected');if(value.dataset.item)value.setAttribute('aria-selected','false');}
  for(const id of ['add-selected','move-screen','move-out'])document.getElementById(id).disabled=true;
  if(document.activeElement?.matches('.shortcut,.large-folder'))document.activeElement.blur();
}
function shortcut(id, sourceFolder = null, compact = false) {
  const item = byId.get(id), button = node("button", "shortcut" + (compact ? " folder-app" : sourceFolder ? " folder-shortcut" : " root-shortcut"));
  button.type = "button"; button.dataset.item = id; button.title = item.name; button.setAttribute("aria-label", item.name); button.draggable = false;
  button.append(icon(id)); if (!compact) button.append(node("span", "app-name", item.name)); button.classList.toggle("selected",selection.has(id,selectionScope(sourceFolder)));button.setAttribute("aria-selected",String(selection.has(id,selectionScope(sourceFolder))));
  button.addEventListener("click", event => { event.stopPropagation(); select(id, sourceFolder,event.ctrlKey);if(event.detail===1)void keyboard?.take(button); });
  button.addEventListener("dblclick", event => { event.stopPropagation();keyboard?.cancel();void mutate("open", { itemId: id }, "已请求打开：" + (byId.get(id)?.name??item.name)); });
  button.addEventListener("contextmenu",event=>{event.preventDefault();event.stopPropagation();select(id,sourceFolder,false,true);showItemMenu(id,event.clientX,event.clientY,sourceFolder,button);});
  button.addEventListener("pointerdown", event => {
    if (event.button !== 0 || model.readOnly || compact && !event.target.closest(".icon,.mini-icon")) return;
    event.stopPropagation(); clearGesture(); hideMenu(); const members=selection.has(id,selectionScope(sourceFolder))?selection.ids:event.ctrlKey&&selection.scope===selectionScope(sourceFolder)?[...selection.ids,id]:[id];const rect = button.getBoundingClientRect();
    gesture = { kind: "item", itemId: id,itemIds:members,virtual:members.some(id=>byId.get(id)?.isVirtual), sourceFolder, pointerId: event.pointerId, x: event.clientX, y: event.clientY, offset: sourceFolder ? { x: cell() / 2, y: cell() / 2 } : grip(event,rect), source: button, active: false };
  }); return button;
}
function folderCard(folder) {
  const card = node("article", "large-folder"), footprint = board.placements[folderKey(folder.id)]; card.dataset.folder = folder.id; card.dataset.tile = folderKey(folder.id); card.tabIndex = 0; card.setAttribute("aria-label", folder.name + "，右键调整布局");
  const cover = node("div", "folder-cover"); cover.style.gridTemplateColumns = `repeat(${footprint.w},minmax(0,1fr))`; cover.style.gridTemplateRows = `repeat(${footprint.h},minmax(0,1fr))`;
  const content = folderCover(folder.itemIds, footprint.w, footprint.h);
  for (const id of content.direct) cover.append(shortcut(id, folder.id, true));
  for (let index = 0; index < content.empty; index++) cover.append(node("div", "empty-slot"));
  const more = node("button", "mini-open"); more.type = "button"; more.setAttribute("aria-label", "展开" + folder.name);
  for (const id of content.preview) more.append(icon(id, true));
  if (content.plus) more.append(node("span", "empty-plus", "+"));
  if (content.overflow) more.append(node("span", "more-count", "+" + content.overflow));
  more.addEventListener("click", () => openFolder(folder.id)); cover.append(more);
  const title = node("button", "folder-name", folder.name + " (" + folder.itemIds.length + ")"); title.type = "button"; title.addEventListener("click", () => openFolder(folder.id)); card.append(cover, title);
  card.addEventListener("pointerdown", event => {
    if (event.button !== 0 || model.readOnly || event.target.closest(".icon,.mini-icon,.more-count,.empty-plus")) return;
    clearGesture();hideMenu();clearSelection();selectedTile=folderKey(folder.id);selected=null;
    document.getElementById("move-screen").disabled = document.getElementById("screen-target").options.length === 0;
    const rect = card.getBoundingClientRect(); gesture = { kind: "folder", folderId: folder.id, pointerId: event.pointerId, x: event.clientX, y: event.clientY, offset: grip(event,rect), source: card, active: false };
  });
  card.addEventListener("contextmenu", event => { event.preventDefault(); showMenu(folder.id, event.clientX, event.clientY); });
  card.addEventListener("keydown", event => { if (event.key === "ContextMenu" || event.shiftKey && event.key === "F10") { event.preventDefault(); const rect = card.getBoundingClientRect(); showMenu(folder.id, rect.left + 12, rect.top + 12); } }); return card;
}
const itemMenu=node("div","folder-menu");itemMenu.hidden=true;itemMenu.setAttribute("role","menu");document.body.append(itemMenu);
let cardMenu=null;
function showCardMenu(card,x,y,source){
 claimMenu();clearGesture();if(!cardMenu){cardMenu=node('div','folder-menu');cardMenu.hidden=true;cardMenu.setAttribute('role','menu');document.body.append(cardMenu);}cardMenu.replaceChildren();
 if(card.type==='system.todo'){
  cardMenu.dataset.desktopCardMenuId=card.id;
  const state=todoCardMenuState(card.id);
  const special=(title,shortcut,command,disabled=false)=>{
   const button=node('button',null);button.type='button';button.setAttribute('role','menuitem');button.disabled=model.readOnly||disabled;
   const label=node('span','windows-menu-label',title);button.append(label);
   if(shortcut){const hint=node('span','windows-menu-shortcut',shortcut);button.append(hint);}
   button.addEventListener('click',()=>{hideMenu();Promise.resolve(runTodoCardMenuAction(card.id,command)).catch(error=>message(error.message,true));});
   cardMenu.append(button);return button;
  };
  special(state.canUndo?'撤销 '+state.undoLabel:'撤销','Ctrl+Z','undo',!state.canUndo);
  special('编辑待办','E','edit');
  cardMenu.append(node('div','windows-menu-separator'));
  special('清除全部已完成',null,'clear',!state.hasCompleted);
 }else delete cardMenu.dataset.desktopCardMenuId;
 const action=(title,method,...values)=>{const button=node('button',null,title);button.disabled=model.readOnly;button.setAttribute('role','menuitem');button.addEventListener('click',()=>{hideMenu();void cardAction(method,...values).catch(error=>message(error.message,true));});cardMenu.append(button);return button;};
 if(card.type==='system.clock')for(const size of CLOCK_SIZES){const button=action('时钟字号：'+size.label,'configure',card.id,{...card.settings,fontSize:size.value});button.setAttribute('role','menuitemradio');button.setAttribute('aria-checked',String(clockSize(card.settings)===size.value));}
 const definition=model.cardDefinitions?.find(value=>value.type===card.type);for(const size of definition?.sizes??[])action(size.rows+'行 × '+size.columns+'列','resize',card.id,size.columns,size.rows);
 for(const screen of model.snapshot.boards.filter(row=>row.active&&row.id!==board.boardId))action('移动到 '+screen.name,'moveScreen',card.id,screen.id);
 action('移除卡片','remove',card.id);
 cardMenu.style.maxHeight='calc(100vh - 16px)';cardMenu.style.overflowY='auto';cardMenu.hidden=false;
 let left=x,top=y;
 if(left+cardMenu.offsetWidth>innerWidth-8)left=x-cardMenu.offsetWidth;
 if(top+cardMenu.offsetHeight>innerHeight-8)top=y-cardMenu.offsetHeight;
 cardMenu.style.left=Math.max(8,Math.min(left,innerWidth-cardMenu.offsetWidth-8))+'px';
 cardMenu.style.top=Math.max(8,Math.min(top,innerHeight-cardMenu.offsetHeight-8))+'px';
 cardMenu.querySelector('button:not(:disabled)')?.focus();queueRegions();
}function claimMenu() { menus?.claim();hideMenu(); }
function hideMenu() { pages.closeMenu();windowsMenu?.close();menu.hidden = true;desktopMenu.hidden=true;itemMenu.hidden=true;if(cardMenu)cardMenu.hidden=true; menuFolder = null;queueRegions(); }
document.addEventListener('magic-card-menu-close',event=>{if(cardMenu&&!cardMenu.hidden&&cardMenu.dataset.desktopCardMenuId===event.detail?.cardId)hideMenu();});
function screenMenuItems(container,tileId){
  for(const screen of model.snapshot.boards.filter(row=>row.active&&row.id!==board.boardId)){
    const button=node("button","move-screen-option","移动到 "+screen.name);button.type="button";
    button.addEventListener("click",()=>{hideMenu();void mutate("move-screen",{tileId,boardId:screen.id});});container.append(button);
  }
}
function showItemMenu(id,x,y,sourceFolder,anchor){
  clearGesture();const items=selectedItems();
  const extras=items.length===1?[{label:'在资源管理器中显示',key:'open',run:()=>request('/action',{monitorId,kind:'reveal',itemId:id})}]:[];
  if(items.length===1&&!sourceFolder)for(const screen of model.snapshot.boards.filter(row=>row.active&&row.id!==board.boardId))extras.push({label:'移动到 '+screen.name,key:'display',run:()=>mutate('move-screen',{tileId:itemKey(id),boardId:screen.id})});
  void windowsMenu?.show(x,y,{item:byId.get(id),items,anchor,extras});
}
function showMenu(id, x, y) {
  claimMenu();
  menu.querySelectorAll(".move-screen-option").forEach(node=>node.remove());screenMenuItems(menu,folderKey(id));
  clearGesture(); menuFolder = id; selectedTile = folderKey(id); menu.hidden = false; const rect = board.placements[selectedTile];
  for (const button of menu.querySelectorAll("button")) { button.disabled = model.readOnly; if (button.dataset.size) button.setAttribute("aria-checked", String(button.dataset.size === `${rect.w}x${rect.h}`)); }
  menu.style.left = Math.max(8, Math.min(x, innerWidth - menu.offsetWidth - 8)) + "px"; menu.style.top = Math.max(8, Math.min(y, innerHeight - menu.offsetHeight - 8)) + "px"; menu.querySelector("button").focus();queueRegions();
}
function openFolder(id) {
  clearSelection();openFolderId=id;renderFolder();
  motion.open(() => {
    const card = [...grid.children].find(element => element.dataset.folder === id);
    return card?.querySelector(".folder-cover") ?? card;
  });
  folderContents.reveal();queueRegions();
}
function renderFolder() {
  const folder = board.folders.find(row => row.id === openFolderId); if (!folder) { void motion.close(true); return; }
  document.getElementById("folder-title").textContent = folder.name; document.getElementById("folder-count").textContent = folder.itemIds.length + " 项";
  folderContents.update(folder);document.getElementById("empty-folder").hidden = folder.itemIds.length > 0;
  document.getElementById("move-out").disabled = model.readOnly || !folder.itemIds.includes(selected);
}
function render() {
  document.body.dataset.takeover=String(!!model.takeoverActive);
  cardHost.setActive(!!model.takeoverActive);
  grid.replaceChildren(...board.rootIds.map(id=>{const value=shortcut(id);value.dataset.tile=itemKey(id);return value;}),...board.folders.filter(folder=>!model.hiddenFolders.includes(folder.id)).map(folderCard),...cardHost.sync(board.cards,model.cardDefinitions,model.honorServices));
  document.getElementById("screen").textContent = model.boardName; document.getElementById("version").textContent = model.version;
  const destination = document.getElementById("destination"), previous = destination.value; destination.replaceChildren(...board.folders.map(folder => { const option = node("option", null, folder.name); option.value = folder.id; return option; })); if (board.folders.some(folder => folder.id === previous)) destination.value = previous;
  const screens = document.getElementById("screen-target"), old = screens.value; screens.replaceChildren(...model.snapshot.boards.filter(row => row.active && row.id !== board.boardId).map(row => { const option = node("option", null, row.name); option.value = row.id; return option; })); if ([...screens.options].some(row => row.value === old)) screens.value = old;
  document.getElementById("move-screen").disabled = model.readOnly || !selectedTile || !screens.options.length;
  document.getElementById("add-selected").disabled = model.readOnly || !selected || !board.rootIds.includes(selected) || !board.folders.length;
  document.getElementById("new-folder").disabled = model.readOnly;
  document.getElementById("item-count").textContent = board.rootIds.length + " 个独立图标 · " + board.folders.length + " 个大文件夹";
  const visible=selection.scope==='desktop'?board.rootIds:board.folders.find(folder=>'folder:'+folder.id===selection.scope)?.itemIds??[];selection.prune(visible);
  folderContents.prune(board.folders);geometry();pages.sync();if(dialog.open)renderFolder();paintSelection();
}
function geometry() {
  if (!board) return;
  scale=1;const p=model.projection;
  for (const value of grid.children) {
    const rect = board.placements[value.dataset.tile];let width=rect.w*p.stepX-p.inset,height=rect.h*p.stepY-p.inset;const overlayHeight=Number(value.dataset.cardOverlayHeight);if(Number.isFinite(overlayHeight)&&overlayHeight>height)height=overlayHeight;value.style.left = rect.x*p.stepX+"px";value.style.top=rect.y*p.stepY+"px";value.style.width=width+"px";value.style.height=height+"px";
    for (const [key, number] of Object.entries(rect)) value.dataset["grid" + key.toUpperCase()] = number;
  }
  grid.style.left=p.originX+"px";grid.style.top=p.originY+"px";grid.style.width=board.columns*p.stepX+"px";grid.style.height=board.rows*p.stepY+"px";grid.style.setProperty("--preview-scale",1);
  queueRegions();
}
async function update(force = false, duringDrag = false) {
  if (updating) { await updating; if (!force) return; }
  updating = (async () => {
    const next = await request("/grid-state?monitor=" + encodeURIComponent(monitorId));
    requestWorkspaceRefresh(next.workspaceRevision);
    if (gesture&&!duringDrag||marquee?.active||windowsMenu?.renaming) { pendingUpdate = true; changeVersion = next.changeVersion; return; }
    if (model && next.revision < model.revision) return;
    const changed = !model || next.revision !== model.revision || next.nativeVersion!==model.nativeVersion || next.takeoverActive!==model.takeoverActive || next.cardCatalogVersion!==model.cardCatalogVersion;
    model = next; board = next.board; changeVersion = next.changeVersion; byId = new Map(next.snapshot.items.map(item => [item.id, item]));
    if (changed || force) {render();if(model.collectionProblem)message(model.collectionProblem,true);else if(model.hiddenFolders.length)message("有文件夹位置被原生图标占用；右键“＋”可重新寻找空位。",true);} else document.querySelectorAll("[data-icon-key]").forEach(updateIcon);
  })();
  try { await updating; } finally { updating = null; }
}
function clearGesture() {
  pageMotion.cancel();pages.cancel();outbound?.cancel();
  if(crossPublished){publishCross({type:"clear"});crossPublished=false;}
  const prior = gesture; gesture = null; ghost?.remove(); ghost = null;preview?.remove();preview=null;dropTarget?.classList.remove("drop-target");dropTarget=null;if(dragFrame)cancelAnimationFrame(dragFrame);dragFrame=0;dragPoint=null;
  if (prior) { prior.source.classList.remove("is-dragging"); if (document.documentElement.hasPointerCapture(prior.pointerId)) document.documentElement.releasePointerCapture(prior.pointerId); }
  document.querySelectorAll(".drop-target").forEach(value => value.classList.remove("drop-target"));
  if (pendingUpdate) { pendingUpdate = false; void update(); }
  queueRegions();
}
function destinationAt(x, y) { const hit = document.elementFromPoint(x, y);const itemNode=hit?.closest('[data-item]');return { itemNode,item:itemNode?byId.get(itemNode.dataset.item):null,folder: hit?.closest("[data-folder]"), inGrid: x>=0&&x<innerWidth&&y>=0&&y<innerHeight&&!hit?.closest(".desktop-accessory-actions,.folder-menu,.accessory-toast,.desktop-pager,.desktop-page-edge,.desktop-unplaced") }; }
function positionAt(x,y,offset){return gridPosition(x,y,offset,model.projection,viewport.scrollLeft,viewport.scrollTop);}
document.addEventListener("pointermove", event => {
  if (!gesture || gesture.pointerId !== event.pointerId) return;
  if (!(event.buttons & 1)) { clearGesture(); return; }
  if (!gesture.active) {
    if (Math.hypot(event.clientX - gesture.x, event.clientY - gesture.y) < 6) return;
    gesture.sourcePageId=board.pageId;gesture.sourceBoard=board;gesture.active=true;if(gesture.kind==='item'){selection.replace(selectionScope(gesture.sourceFolder),dragMembers(gesture));paintSelection();}gesture.source.classList.add("is-dragging"); document.documentElement.setPointerCapture(event.pointerId); void motion.close(true);queueRegions();
    ghost = node("div", "drag-ghost"); ghost.append(gesture.kind !== "item" ? node("span", "folder-ghost", gesture.kind==='card'?"移动卡片":"移动文件夹") : icon(gesture.itemId));if(gesture.kind==='item'&&dragMembers(gesture).length>1)ghost.append(node("span","drag-count",dragMembers(gesture).length+" 项")); document.body.append(ghost);
  }
  event.preventDefault();pages.pointer(event.clientX,event.clientY);void outbound?.check(gesture);dragPoint={x:event.clientX,y:event.clientY};
  if(!dragFrame)dragFrame=requestAnimationFrame(()=>{
    dragFrame=0;if(!gesture||!dragPoint)return;
    const {x,y}=dragPoint;ghost.style.transform=`translate(${x+12}px,${y+12}px)`;
    if(nativeFrame&&(x<0||y<0||x>=innerWidth||y>=innerHeight)){
      const span=gesture.kind!=='item'?gesture.sourceBoard.placements[tileKey(gesture)]:{w:1,h:1};
      publishCross({type:"preview",kind:gesture.kind,itemId:gesture.itemId,point:{x:nativeFrame.rect.left+x*nativeFrame.scale,y:nativeFrame.rect.top+y*nativeFrame.scale},offset:gesture.offset,span:{w:span.w,h:span.h},offsets:gesture.kind==='item'?groupOffsets(gesture,gesture.sourceBoard):undefined,itemIds:gesture.itemIds});crossPublished=true;
      if(preview)preview.hidden=true;dropTarget?.classList.remove("drop-target");dropTarget=null;return;
    }
    if(crossPublished){publishCross({type:"clear"});crossPublished=false;}
    const destination=destinationAt(x,y),key=tileKey(gesture);
    const physical=gesture.kind==="item"?(!dragMembers(gesture).includes(destination.item?.id)?fileDropTarget(gesture.itemId,destination.item):null):null;
    const target=gesture.kind==="item"?(physical?destination.itemNode:destination.folder):null;
    if(dropTarget!==target){dropTarget?.classList.remove("drop-target");dropTarget=target;dropTarget?.classList.add("drop-target");}
    if(target){if(preview)preview.hidden=true;return;}
    const source=gesture.kind!=='item'?gesture.sourceBoard.placements[key]:{w:1,h:1};
    const position=positionAt(x,y,gesture.offset),crossPage=gesture.sourcePageId&&gesture.sourcePageId!==board.pageId,offsets=gesture.kind==='item'?groupOffsets(gesture,gesture.sourceBoard):[{dx:0,dy:0,w:source.w,h:source.h}],group=crossPage?nearestGroupPlacement(position,offsets,board):gesture.kind==='item'&&dragMembers(gesture).length>1?groupPlacement(position,offsets,board,dragMembers(gesture).map(itemKey)):null;const rect=group?.rect??{...position,w:source.w,h:source.h},error=group?.error??(group?null:placementError(rect,board,key)),f=gridFrame(rect,model.projection);
    if(!preview){preview=node("div","grid-drop-preview");document.body.append(preview);}
    preview.hidden=false;preview.dataset.invalid=String(!!error);preview.style.width=f.width+"px";preview.style.height=f.height+"px";preview.style.transform=`translate(${f.left-viewport.scrollLeft}px,${f.top-viewport.scrollTop}px)`;
  });
}, { passive: false });
document.addEventListener("pointerup", async event => {
  if (!gesture || gesture.pointerId !== event.pointerId) return;
  if (!gesture.active) { clearGesture(); return; }
  event.preventDefault(); suppressClicksUntil = performance.now() + 200;
  if(pageChangePromise)await pageChangePromise;
  if(!gesture||gesture.pointerId!==event.pointerId)return;
  const prior = { ...gesture }, destination = destinationAt(event.clientX, event.clientY), position = positionAt(event.clientX, event.clientY, prior.offset); clearGesture();
  if(await outbound?.guardDrop(prior))return;
  if (event.clientX<0||event.clientX>=innerWidth||event.clientY<0||event.clientY>=innerHeight) {
    const tileId=tileKey(prior);
    void mutate("cross-drop",{tileId,sourcePageId:prior.sourcePageId,itemIds:prior.itemIds,control:event.ctrlKey,shift:event.shiftKey,sourceFolder:prior.sourceFolder??null,clientX:event.clientX,clientY:event.clientY,offsetX:prior.offset.x,offsetY:prior.offset.y},"已移动到另一屏，文件路径保持不变。");return;
  }
  if (!destination.inGrid) { message("移动已取消，请放到桌面网格或文件夹内。"); return; }
  const physical=prior.kind==='item'?(!dragMembers(prior).includes(destination.item?.id)?fileDropTarget(prior.itemId,destination.item):null):null;
  if(physical){if(dragMembers(prior).length>1)void groupFileOperation(prior.itemIds,physical.targetId,event.ctrlKey,event.shiftKey);else void fileOperation(physical.action,prior.itemId,{targetId:physical.targetId});return;}
  const destinationFolder=prior.kind==="item"?destination.folder?.dataset.folder:null;
  if(prior.sourcePageId&&prior.sourcePageId!==board.pageId){
    void mutate('page-drop',{tileId:tileKey(prior),tileIds:prior.kind==='item'?dragMembers(prior).map(itemKey):[tileKey(prior)],sourcePageId:prior.sourcePageId,destinationPageId:board.pageId,sourceFolder:prior.sourceFolder??null,destinationFolder:destinationFolder||null,...position});return;
  }
  if(prior.kind==='item'&&dragMembers(prior).length>1){
    if(!destinationFolder){const placed=groupPlacement(position,groupOffsets(prior,board),board,dragMembers(prior).map(itemKey));if(placed.error){message(placed.error,true);return;}}
    void mutate('group-reference',{itemId:prior.itemId,itemIds:prior.itemIds,sourceFolder:prior.sourceFolder,destinationFolder:destinationFolder||null,...position});return;
  }
  if(!destinationFolder){const key=tileKey(prior),source=prior.kind!=='item'?board.placements[key]:{w:1,h:1};const error=placementError({...position,w:source.w,h:source.h},board,key);if(error){message(error,true);return;}}
  if (prior.kind !== "item") void mutate("place", { tileId: tileKey(prior), ...position });
  else void mutate("reference", { itemId: prior.itemId, sourceFolder: prior.sourceFolder, destinationFolder: destinationFolder || null,...position });
});
document.addEventListener("pointercancel", clearGesture);document.documentElement.addEventListener("lostpointercapture",event=>{if(gesture?.pointerId===event.pointerId)clearGesture();});window.addEventListener("blur", ()=>{clearGesture();hideMenu();});
document.addEventListener("click", event => { if (suppressMarqueeClick||performance.now()<suppressClicksUntil) { suppressMarqueeClick=false;event.preventDefault();event.stopPropagation();} }, true);
document.addEventListener("pointerdown",()=>{if(!marquee?.active)suppressMarqueeClick=false;suppressClicksUntil=0;},true);
document.addEventListener("pointerdown", event => {
  menus?.claim();
  const inMenu=windowsMenu?.contains(event.target)||menu.contains(event.target)||desktopMenu.contains(event.target)||itemMenu.contains(event.target)||cardMenu?.contains(event.target)||!!event.target.closest(".desktop-page-menu");
  if(!inMenu&&!event.target.closest('[data-folder]'))hideMenu();
  if(event.button===0&&!event.ctrlKey&&!inMenu&&!event.target.closest('[data-tile],button,input,textarea,select,[data-inline-rename]')&&(event.target.closest('#grid-viewport,.folder-dialog')||event.target===document.body||event.target===document.documentElement))clearSelection();
});
document.addEventListener('keydown',event=>{
  if(event.defaultPrevented||event.target.closest('input,textarea,select,[contenteditable=true],[role=menu]'))return;
  if(event.key==='Escape'){hideMenu();clearGesture();return;}
  if(!dialog.open&&!cardManager.isOpen&&pages.key(event))return;
  const scope=dialog.open?'folder:'+openFolderId:'desktop';
  if(event.ctrlKey&&!event.altKey&&event.key.toLowerCase()==='a'){event.preventDefault();selection.replace(scope,dialog.open?board.folders.find(f=>f.id===openFolderId)?.itemIds??[]:board.rootIds);paintSelection();return;}
  if(event.key==='F2'&&selection.size===1&&!model.readOnly){event.preventDefault();const item=selectedItems()[0],anchor=dialog.open?folderContents.anchor(item.id):[...grid.querySelectorAll('[data-item]')].find(el=>el.dataset.item===item.id&&selection.has(item.id,scope));if(!item.isVirtual){hideMenu();void windowsMenu?.rename(item,anchor);}return;}
  if(event.key==='Enter'&&!event.altKey&&!event.ctrlKey&&selection.size){event.preventDefault();keyboard?.cancel();if(selection.size===1)void mutate('open',{itemId:selection.primary});else void windowsMenu?.command('open',selectedItems());return;}
  const key=event.key.toLowerCase(),command=event.ctrlKey&&!event.altKey?({c:'copy',x:'cut',v:'paste'})[key]:key==='delete'?'delete':event.altKey&&key==='enter'?'properties':null;
  if(command&&(command==='paste'||selection.size)){event.preventDefault();void windowsMenu?.command(command,selectedItems());return;}
  if((event.key==='ContextMenu'||event.shiftKey&&event.key==='F10')&&selection.size){event.preventDefault();const anchor=dialog.open?folderContents.anchor(selection.primary):[...grid.querySelectorAll('[data-item]')].find(el=>selection.has(el.dataset.item,scope));if(anchor){const r=anchor.getBoundingClientRect();void windowsMenu?.show(r.left+12,r.top+12,{item:byId.get(anchor.dataset.item),items:selectedItems(),anchor});}}
});
for (const [columns, rows] of sizes) {
  const button = node("button", null, `${rows}行 × ${columns}列`); button.dataset.size = `${columns}x${rows}`; button.setAttribute("role", "menuitemradio");
  button.addEventListener("click", () => { const folderId = menuFolder; hideMenu(); void mutate("resize", { folderId, columns, rows }, `已切换为 ${rows}行 × ${columns}列。`); }); menu.append(button);
}
const dissolve = node("button", "dissolve-folder", "解散大文件夹"); dissolve.setAttribute("role", "menuitem"); dissolve.addEventListener("click", () => { const folderId = menuFolder; hideMenu(); void mutate("dissolve", { folderId }, "已解散；项目回到桌面网格，文件路径保持不变。"); }); menu.append(dissolve);
document.getElementById("close-folder").addEventListener("click", () => { void motion.close(); });
dialog.addEventListener("cancel", event => { event.preventDefault(); void motion.close(); });
dialog.addEventListener("close", () => { clearSelection();openFolderId = null; folderContents.hide();queueRegions(); });
dialog.addEventListener("click", event => { const rect = dialog.getBoundingClientRect(); if (event.target === dialog && (event.clientX < rect.left || event.clientX > rect.right || event.clientY < rect.top || event.clientY > rect.bottom)) void motion.close(); });
document.getElementById("move-out").addEventListener("click", () => { if (selected && openFolderId) void mutate("reference", { itemId: selected, sourceFolder: openFolderId, destinationFolder: null },"已移出到桌面网格，文件路径保持不变。"); });
document.getElementById("add-selected").addEventListener("click", () => { if (selected) void mutate("reference", { itemId: selected, sourceFolder: null, destinationFolder: document.getElementById("destination").value }); });
document.getElementById("new-folder").addEventListener("click", event => { void newFolderAt(event.detail?{x:event.clientX,y:event.clientY}:null); });
document.getElementById("move-screen").addEventListener("click", () => { if (selectedTile) void mutate("move-screen", { tileId: selectedTile, boardId: document.getElementById("screen-target").value }); });
document.getElementById("display-scale").addEventListener("change", () => { clearGesture(); hideMenu(); geometry(); }); viewport.addEventListener("scroll", () => {hideMenu();clearRemotePreview();queueRegions();});
const resizeObserver = new ResizeObserver(() => { marquee?.cancel();clearGesture();hideMenu(); geometry(); }); resizeObserver.observe(viewport);
for (const kind of ["refresh", "settings", "feedback", "exit"]) document.getElementById(kind).addEventListener("click", () => { if (kind !== "exit" || confirm("退出 Magic 桌面并返回 Windows？布局和原文件会保留。")) void basic(kind); });
async function watch() {
  let retry = 1000, disconnected = false;
  while (!stopped) {
    const controller=new AbortController();watchController=controller;
    const headerTimeout=setTimeout(()=>controller.abort(),8000);
    try {
      // Headers arrive immediately; an idle event stream is not an unfinished JSON response.
      const response=await fetch(session.endpoint+"/events?after="+changeVersion,{headers:{Authorization:"Bearer "+session.token},signal:controller.signal});clearTimeout(headerTimeout);
      if(!response.ok||!response.body)throw new Error("更新通知连接未就绪。");
      // A successful stream handshake does not prove this monitor is registered.
      // Revalidate once per connection, including when a topology event was missed.
      await update();retry=1000;
      if(disconnected){message("桌面连接已恢复。");disconnected=false;}
      const reader=response.body.getReader(),decoder=new TextDecoder();let buffer="";
      try { while(!stopped){const part=await reader.read();if(part.done)throw new Error("更新通知连接已关闭。");buffer+=decoder.decode(part.value,{stream:true});let end;while((end=buffer.indexOf("\n"))>=0){const line=buffer.slice(0,end);buffer=buffer.slice(end+1);if(line.trim()){const next=JSON.parse(line);if(next.changeVersion!==changeVersion)await update();}}} }
      finally {reader.releaseLock();}
    }
    catch(error) {
      if (stopped) return; console.warn("MAGIC_GRID_WATCH_RETRY", JSON.stringify({name:error.name,message:error.message}));
      if(error.code==='monitor-registration-required'){
        nativeReady=false;regionSignature='';
        if(await queueMonitorRefresh(false,true)) {retry=1000;continue;}
      }
      try { await request("/health"); } catch { disconnected=true;message("桌面连接暂不可用，可从固定入口重新进入。", true); }
      await new Promise(resolve => setTimeout(resolve, retry)); retry = Math.min(30000, retry * 2);
    }
    finally {clearTimeout(headerTimeout);controller.abort();if(watchController===controller)watchController=null;}
  }
}
let topologySignature='',monitorRefresh=Promise.resolve(),monitorRefreshPending=0,appliedWorkspaceRevision=0,workspaceRefreshQueued=false,wantedWorkspaceRevision=0;
const monitorTopology=monitors=>JSON.stringify(monitors.asArray().map(m=>({id:m.id,rect:m.rect,scale:m.scaleFactor})).sort((a,b)=>a.id.localeCompare(b.id)));
const requireActivePage=()=>{if(stopped)throw new DOMException('桌面视图已退出。','AbortError');};
async function reportReady(registration){
  requireActivePage();await widget.window.show();requireActivePage();
  const ready=await request("/ready", { monitorId, widgetId: widget.id, windowId: widget.windowId, origin: location.origin, sdkVersion: "2.8.4",frontendVersion,gridVersion: 3,...regionBody() });requireActivePage();
  // AppBar reservations can change between registration and the final native
  // attachment. Cross-screen coordinates must use the actual attached frame.
  if(ready.rect&&nativeFrame)nativeFrame={...nativeFrame,rect:ready.rect};
  appliedWorkspaceRevision=ready.workspaceRevision??registration?.workspaceRevision??appliedWorkspaceRevision;
  nativeReady=true;await update(true);requireActivePage();regionSignature=JSON.stringify(regionBody());queueRegions();
}
function requestWorkspaceRefresh(revision){
  if(!Number.isSafeInteger(revision)||revision<=appliedWorkspaceRevision||stopped)return;
  wantedWorkspaceRevision=Math.max(wantedWorkspaceRevision,revision);
  if(!nativeReady||workspaceRefreshQueued)return;
  workspaceRefreshQueued=true;
  void queueMonitorRefresh(true).then(attemptedRevision=>{
    workspaceRefreshQueued=false;
    // A revision may arrive after the task finishes but before this promise
    // continuation. Drain that newer event without retrying a failed revision.
    if(!stopped&&wantedWorkspaceRevision>appliedWorkspaceRevision&&wantedWorkspaceRevision>attemptedRevision)requestWorkspaceRefresh(wantedWorkspaceRevision);
  });
}
function queueMonitorRefresh(workspace=false,force=false){
  let attemptedRevision=0,recovered=false;
  monitorRefreshPending++;
  monitorRefresh=monitorRefresh.then(async()=>{
    requireActivePage();
    attemptedRevision=workspace?wantedWorkspaceRevision:0;
    if(workspace&&attemptedRevision<=appliedWorkspaceRevision)return;
    const monitors=await ConnectedMonitorList.getAsync();requireActivePage();
    if((!force&&monitorTopology(monitors)===topologySignature&&(!workspace||attemptedRevision<=appliedWorkspaceRevision))||!monitors.asArray().some(m=>m.id===monitorId))return;
    marquee?.cancel();clearGesture();hideMenu();clearRemotePreview();void cardManager.close(true);void motion.close(true);
    const registration=await register(monitors);await reportReady(registration);topologySignature=registration.topology;recovered=true;
    if(workspace&&appliedWorkspaceRevision<attemptedRevision)throw new Error('工作区恢复尚未确认，请在下一次恢复通知后重试。');
  }).catch(error=>{if(!stopped)message(error.message,true);}).then(()=>force?recovered:attemptedRevision).finally(()=>{monitorRefreshPending--;});
  return monitorRefresh;
}
async function register(monitors = null) {
  requireActivePage();monitors??=await ConnectedMonitorList.getAsync();requireActivePage();const monitor = monitors.asArray().find(value => value.id === monitorId); if (!monitor) throw new Error("当前显示器已断开。");
  const registration = await request("/register", { monitorId, rect: monitor.rect, scale: monitor.scaleFactor });requireActivePage();nativeFrame={rect:registration.rect,monitorRect:monitor.rect,scale:monitor.scaleFactor};await widget.setPosition(registration.rect);requireActivePage();
  await request("/prepare-window",{monitorId,widgetId:widget.id,windowId:widget.windowId});requireActivePage();await update(true);requireActivePage();return {...registration,topology:monitorTopology(monitors)};
}
async function main() {
  await widget.init({ useThemes: false, saveAndRestoreLastRect: false });
  // Preset None preserves our verified Explorer ownership/composition. Keep the
  // host's cached window flags in sync too: focusing can reapply those flags and
  // otherwise restore its original titlebar, border and taskbar button.
  await widget.window.setDecorations(false);await widget.window.setShadow(false);await widget.window.setResizable(false);
  await widget.window.setMinimizable(false);await widget.window.setMaximizable(false);await widget.window.setClosable(false);
  await widget.window.setSkipTaskbar(true);await widget.window.setAlwaysOnBottom(true);
  const registration = await register(); await widget.ready();
  // Readiness must be renewed after topology changes, not just initial mount.
  await reportReady(registration);topologySignature=registration.topology;
  console.info("MAGIC_DESKTOP_GRID_READY", JSON.stringify({ widgetId: widget.id, monitorId, windowId: widget.windowId, version: session.version, rect: registration.rect, gridVersion: 3, nativeIconTakeover: true }));
  const unlistenMonitors=await ConnectedMonitorList.onChange(monitors => {
    // Brightness/HDR changes share this event. Discard them from the payload
    // before another native read or any positioning/ownership work is queued.
    if(stopped||(!monitorRefreshPending&&monitorTopology(monitors)===topologySignature))return;
    void queueMonitorRefresh();
  });
  if(stopped){unlistenMonitors?.();return;}monitorUnsubscribe=unlistenMonitors;
  // Cache configuration once, then only react to settings changes. Menus do
  // not query or poll the native Dock, whose WebView is much larger than its hitbox.
  try{
    const unlisten=await Settings.onChange(value=>{desktopSettings=value.inner;windowsMenu?.close();});
    if(stopped){unlisten();return;}settingsUnsubscribe=unlisten;
    const settings=await Settings.getAsync();requireActivePage();desktopSettings??=settings.inner;
  }catch(error){if(stopped)return;console.warn('MAGIC_DESKTOP_MENU_BOUNDS',error.message);}
  externalDrop=installExternalDrop({document,nativeListen:handler=>getCurrentWebview().onDragDropEvent(handler),readOnly:()=>model.readOnly,pixelRatio:()=>devicePixelRatio,
    folderAt:(x,y)=>{if(dialog.open){const r=dialog.getBoundingClientRect();return x>=r.left&&x<r.right&&y>=r.top&&y<r.bottom?openFolderId:null;}return document.elementFromPoint(x,y)?.closest("[data-folder]")?.dataset.folder??null;},
    desktopAt:(x,y)=>!dialog.open&&x>=0&&y>=0&&x<innerWidth&&y<innerHeight&&!document.elementFromPoint(x,y)?.closest('[data-tile],button,[role="menu"],.desktop-accessory-actions'),
    importFiles:async paths=>{if(busy)throw new Error('上一项操作未完成，请稍候再拖入。');busy=true;try{await request('/desktop-import-files',{monitorId,paths},0);message('Windows 已处理传入文件，桌面已更新。');}finally{busy=false;void update(true).catch(error=>message(error.message,true));}},
    highlight:id=>{for(const card of grid.children)card.classList.toggle("drop-target",card.dataset.folder===id);},
    commit:(kind,values)=>mutate(kind,values,"已加入虚拟分组，文件路径保持不变。"),message,
    report:(channel,phase)=>request("/drop-diagnostic",{monitorId,channel,phase})});await externalDrop.ready;
  keyboard=desktopKeyboard({document,windowFocus:()=>widget.window.setFocus(),webviewFocus:()=>getCurrentWebview().setFocus(),verify:()=>request('/desktop-keyboard-state',{monitorId}),message,trace:phase=>console.info('MAGIC_DESKTOP_KEYBOARD',JSON.stringify({phase,monitorId,frontendVersion}))});
  outbound=outboundDrag({current:()=>gesture,probe:()=>request('/desktop-drag-probe',{monitorId},750),message,handoff:prior=>{
    const itemId=prior.itemId,itemIds=prior.itemIds;clearGesture();keyboard.cancel();suppressClicksUntil=performance.now()+500;
    if(busy||model.readOnly){message('上一项操作未完成，请稍后重新拖动。',true);return;}busy=true;
    void invoke('magic_start_desktop_file_drag',{itemId,...(itemIds?.length>1?{itemIds}:{})}).then(effect=>{if(effect)message(effect&2?'Windows 已处理文件移动。':'Windows 已处理文件复制。');}).catch(error=>message(error.message,true)).finally(()=>{busy=false;suppressClicksUntil=performance.now()+200;void update(true).catch(error=>message(error.message,true));});
  }});
  windowsMenu=windowsDesktopMenu({document,request:(url,values)=>request(url,{monitorId,...values},['/desktop-context-invoke','/desktop-properties','/desktop-clipboard-command'].includes(url)?0:15000),claim:claimMenu,fileOperation,focusForEdit:anchor=>keyboard.take(anchor),
    createItem:createDesktopItem,placementFor:creationValues,bounds:()=>desktopMenuBounds({width:innerWidth,height:innerHeight,frame:nativeFrame,settings:desktopSettings,monitorId}),canCommand:key=>!busy&&(!model.readOnly||!['cut','paste','delete','rename'].includes(key)),
    editEnded:()=>{if(pendingUpdate){pendingUpdate=false;void update(true).catch(error=>message(error.message,true));}},
    magic:(kind,point)=>{if(kind==='new-card')void cardManager.open().catch(error=>message(error.message,true));else if(kind==="new-folder")void newFolderAt(point);else if(kind==="reposition-folders")void mutate(kind);else if(kind!=="exit"||confirm("退出 Magic 桌面并返回 Windows？"))void basic(kind);},message,changed:queueRegions});
  marquee=installMarquee({document,window,viewport,grid,dialog,state:selection,paint:paintSelection,closeMenus:claimMenu,blocked:()=>busy||!!gesture||!!windowsMenu?.renaming,focus:anchor=>keyboard.take(anchor,{quiet:true,activate:false}),finished:(active,cancelled)=>{suppressMarqueeClick=active&&!cancelled;if(pendingUpdate){pendingUpdate=false;void update(true).catch(error=>message(error.message,true));}}});
  menus=menuChannel({sessionId:model.sessionId,monitorId,dismiss:hideMenu});
  crossChannel=crossPreviewChannel({sessionId:model.sessionId,monitorId,receive:receiveCrossPreview,diagnostic:phase=>{void request("/drop-diagnostic",{monitorId,channel:"cross-preview",phase}).catch(error=>console.warn("MAGIC_CROSS_PREVIEW",error.message));}});
  message("大文件夹为虚拟分组；拖入真实文件夹会移动文件，拖入回收站会回收。双击打开，右键查看更多操作。"); void watch();
}
window.addEventListener("pagehide", () => { stopped = true;folderContents.dispose();pageMotion.dispose();pages.dispose();cardManager.dispose();cardHost.dispose();cardData.dispose();monitorUnsubscribe?.();settingsUnsubscribe?.();marquee?.dispose();clearGesture();clearRemotePreview();crossChannel?.close();menus?.close();windowsMenu?.dispose();keyboard?.dispose();outbound?.dispose();watchController?.abort(); externalDrop?.dispose();if(regionFrame)cancelAnimationFrame(regionFrame);motion.cancel(); resizeObserver.disconnect(); });
main().catch(error => {if(stopped)return;message("桌面连接失败：" + error.message + "。请从固定入口重试。", true); console.error("MAGIC_DESKTOP_FAILED", error.message); });
import {installScrollbars} from '../shared/overlay-scrollbars.js';
installScrollbars();

viewport.addEventListener("contextmenu",event=>{
  if(event.target.closest("[data-tile],.desktop-pager,.desktop-page-edge,.desktop-page-menu,.desktop-unplaced"))return;
  event.preventDefault();clearSelection();clearGesture();void windowsMenu?.show(event.clientX,event.clientY);
});
