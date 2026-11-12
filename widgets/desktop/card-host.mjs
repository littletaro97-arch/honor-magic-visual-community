// No observer, subscription, timer or renderer exists until there is a card.
export function createDesktopCardHost({document,renderers,dataHub,configure,shortcut=async()=>{},focus=async()=>false,openStorageCleanup=async()=>{throw new Error('管家清理入口不可用。');},onError=console.error,onDragStart=()=>{},onContextMenu=()=>{}}){
 const instances=new Map();let observer=null,listening=false,enabled=true,definitions=new Map(),honorServices=false;
 const node=(tag,cls,text)=>{const element=document.createElement(tag);element.className=cls;if(text!==undefined)element.textContent=text;return element;};
 const failure=error=>{try{onError(error);}catch{}};
 function stop(entry){const prior=entry.live;entry.live=null;if(!prior)return;prior.abort.abort();try{prior.dispose?.();}catch(error){failure(error);}entry.content.replaceChildren();}
 function reconcile(entry){
  const active=enabled&&!document.hidden&&entry.intersects,definition=definitions.get(entry.value.type),factory=renderers.get(entry.value.type,definition);
  if(!active||!definition||!factory){stop(entry);if(!entry.content.firstChild)entry.content.textContent=definition&&factory?'':"卡片暂不可用";return;}
  const signature=JSON.stringify([entry.value,definition,honorServices]);if(entry.live?.signature===signature)return;stop(entry);entry.content.replaceChildren();
  const live={abort:new AbortController(),dispose:null,signature};entry.live=live;
  const context={apiVersion:1,honorServices,instance:structuredClone(entry.value),definition:structuredClone(definition),signal:live.abort.signal,
   openShortcut:index=>live.abort.signal.aborted?Promise.reject(new DOMException('Card is inactive','AbortError')):shortcut(entry.value.id,index),
   openStorageCleanup:()=>live.abort.signal.aborted||entry.value.type!=='system.storage'?Promise.resolve(false):Promise.resolve().then(()=>openStorageCleanup(entry.value.id)).catch(error=>{if(!live.abort.signal.aborted)failure(error);return false;}),
   updateSettings:settings=>live.abort.signal.aborted?Promise.reject(new DOMException('Card is inactive','AbortError')):configure(entry.value.id,settings),requestFocus:()=>live.abort.signal.aborted?Promise.resolve(false):focus(entry.root),
   subscribe:(name,listener)=>{if(!definition.dataSources?.includes(name))throw new Error('Card did not declare this data source');return dataHub.subscribe(name,listener,live.abort.signal);}};
  try{Promise.resolve(factory(entry.content,context)).then(result=>{const dispose=typeof result==='function'?result:result?.dispose?.bind(result);if(result!==undefined&&typeof dispose!=='function')throw new Error('Card renderer must return a disposer');if(live.abort.signal.aborted)dispose?.();else live.dispose=dispose;}).catch(error=>{if(entry.live===live){stop(entry);entry.content.textContent='卡片暂不可用';}failure(error);});}
  catch(error){stop(entry);entry.content.textContent='卡片暂不可用';failure(error);}
 }
 function visibility(){for(const entry of instances.values())reconcile(entry);}
 function begin(){if(listening)return;listening=true;document.addEventListener('visibilitychange',visibility);
  if(globalThis.IntersectionObserver)observer=new IntersectionObserver(changes=>{for(const change of changes){const entry=instances.get(change.target.dataset.card);if(entry){entry.intersects=change.isIntersecting;reconcile(entry);}}});
 }
 function end(){observer?.disconnect();observer=null;if(listening)document.removeEventListener('visibilitychange',visibility);listening=false;}
 function remove(id,entry){observer?.unobserve(entry.root);stop(entry);entry.root.remove();instances.delete(id);}
 return {
  sync(cards=[],catalog=[],services=false){
   honorServices=services===true;
   definitions=new Map(catalog.map(value=>[value.type,value]));const keep=new Set(cards.map(card=>card.id));for(const [id,entry] of instances)if(!keep.has(id))remove(id,entry);
   if(!cards.length){end();return [];}
   begin();const roots=[];
   for(const card of cards){let entry=instances.get(card.id);
    if(!entry){const root=node('article','desktop-card'),header=node('div','desktop-card-drag'),title=node('span','desktop-card-title'),content=node('div','desktop-card-content');root.dataset.tile='card:'+card.id;root.dataset.card=card.id;root.tabIndex=0;header.append(title);root.append(header,content);entry={root,header,title,content,value:card,intersects:!observer,live:null};instances.set(card.id,entry);
     header.title='拖动标题条移动卡片，右键调整尺寸或移除';header.addEventListener('pointerdown',event=>onDragStart(event,entry.value,root));root.addEventListener('contextmenu',event=>onContextMenu(event,entry.value,root));root.addEventListener('keydown',event=>{if(event.key==='ContextMenu'||event.shiftKey&&event.key==='F10')onContextMenu(event,entry.value,root);});observer?.observe(root);
     root.addEventListener('pointerdown',event=>{if(root.dataset.appearance==='widget'&&!event.target.closest('button,input,textarea,select,a,[contenteditable=true]'))onDragStart(event,entry.value,root);});
    }
    const definition=definitions.get(card.type),appearance=renderers.get(card.type,definition)?.appearance;
    entry.root.dataset.appearance=appearance??'card';entry.header.hidden=appearance==='widget';
    entry.value=card;entry.title.textContent=definition?.title??card.type;entry.root.setAttribute('aria-label',entry.title.textContent);reconcile(entry);roots.push(entry.root);
   }
   return roots;
  },
  setActive(value){if(enabled!==value){enabled=value;visibility();}},
  dispose(){for(const [id,entry] of [...instances])remove(id,entry);end();},
  get size(){return instances.size;}
 };
}
