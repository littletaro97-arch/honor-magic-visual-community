"""Import once with --import-from; regenerate only from ordinary project assets."""
from pathlib import Path
import argparse, hashlib, json, re, shutil, xml.etree.ElementTree as ET

root=Path(__file__).resolve().parents[2]
assets=root/'assets/honor-icons'
parser=argparse.ArgumentParser()
parser.add_argument('--import-from',type=Path)
args=parser.parse_args()
def sha(p): return hashlib.sha256(p.read_bytes()).hexdigest()
def write(p,s):
    p.parent.mkdir(parents=True,exist_ok=True)
    p.write_bytes(s.encode('utf-8'))
if args.import_from:
    source=args.import_from.resolve()
    if (assets/'manifest.json').exists(): raise SystemExit('Assets already imported; review an explicit refresh separately.')
    entries=[]
    for p in sorted(source.rglob('*')):
        if not p.is_file() or p.suffix.lower() not in {'.png','.svg','.ico'}: continue
        rel=p.relative_to(source)
        if 'PreviewAnimations' in rel.parts or p.name in {'dandianjujiao.png','lingdongbolang.png'}: continue
        destination=assets/'library'/rel
        destination.parent.mkdir(parents=True,exist_ok=True);shutil.copyfile(p,destination)
        entries.append(dict(path='library/'+rel.as_posix(),original=rel.as_posix(),sha256=sha(destination),bytes=destination.stat().st_size))
    manifest=dict(schemaVersion=1,catalog='Magic.Community.HonorIcons/1',owner='HONOR',importedAt='2026-10-26',
                  provenance='User supplied official icon extraction; original HONOR MagicTaskbarApp Resources',
                  sourceRoot=str(source),files=entries,
                  excluded='Preview animations, two animation sprite sheets, HTML, design tokens and analysis text are not icon assets.')
    write(assets/'manifest.json',json.dumps(manifest,ensure_ascii=False,indent=2)+'\n')
manifest=json.loads((assets/'manifest.json').read_text('utf-8'))
for e in manifest['files']:
    if sha(assets/e['path'])!=e['sha256']: raise SystemExit('Asset hash mismatch: '+e['path'])
white={p.stem:p for p in (assets/'library/DockIcon-白底').glob('*.png')}
clear={p.stem:p for p in (assets/'library/DockIcon-通透').glob('*.png')}
# Exact aliases only. Window titles and partial process names never identify apps.
aliases={
 'Weixin':['WeChat','微信'], 'msedge':['Microsoft Edge'], 'chrome':['Google Chrome','谷歌浏览器'],
 '资源管理器':['explorer','File Explorer','文件资源管理器'], '微软设置':['SystemSettings','Windows Settings','设置'],
 'vscode':['Code','Visual Studio Code'], 'visual-studio':['devenv','Visual Studio'],
 'ps':['Photoshop'],'pycharm64':['pycharm'],'ChatGPT':['ChatGPT'],
 '哔哩哔哩':['bilibili'],'飞书':['Feishu','Lark'],'DingtalkLauncher':['DingTalk','钉钉'],
 'QQ':['QQ'],'Steam':['steam'],'WeMeetApp':['腾讯会议'],'HnAppStore':['荣耀应用市场'],
 'PCManager':['荣耀电脑管家'],'Hihonornote':['荣耀笔记'],'cloudmusic':['网易云音乐'],
 'WINWORD':['Microsoft Word'],'EXCEL':['Microsoft Excel'],'POWERPNT':['Microsoft PowerPoint'],
 'ONENOTE':['Microsoft OneNote'],'7zFM':['7-Zip'],'BaiduNetdisk':['百度网盘'],
 'RecycleBin_ Empty':[]
}
app_entries=[]
# These pairs have different filenames for the same original application.
counterparts={'vscode':'Code','AUTOCADM-sh-min':'autocad-sh-min'}
for name in sorted(set(white)|set(clear)):
    if name in {'RecycleBin_ Empty','RecycleBin_NotEmpty','closetray','opentray'} or name in counterparts.values(): continue
    a=white.get(name);b=clear.get(counterparts.get(name,name))
    # Missing variants in the source sets use the recorded genuine counterpart.
    app_entries.append(dict(id=name,white=(a or b).relative_to(assets).as_posix(),translucent=(b or a).relative_to(assets).as_posix(),
                            aliases=sorted(set([name]+([counterparts[name]] if name in counterparts else [])+aliases.get(name,[])),key=str.casefold),counterpartFallback=not(a and b)))
# Fix the alias target to an existing 7-Zip original.
for e in app_entries:
    if e['id']=='s7z': e['aliases']+=['7zFM','7zG','7-Zip']
catalog=dict(schemaVersion=1,defaultScheme='honor-white',schemes=['honor-white','honor-translucent'],
             applicationMatch='Resolved shortcut target, exact executable, package or Steam identity; no display-name or window-title guessing.',
             apps=app_entries)
write(assets/'catalog.json',json.dumps(catalog,ensure_ascii=False,indent=2)+'\n')
def wide(s): return 'L"'+s.replace('\\','\\\\').replace('"','\\"')+'"'
lines=['#pragma once','// Generated from assets/honor-icons; preserve NOTICE.md and manifest hashes.',
       'struct HonorAppAsset{wchar_t const *alias,*white,*translucent;};','inline constexpr HonorAppAsset HonorAppAssets[]={']
for e in app_entries:
    for alias in e['aliases']: lines.append('    {'+','.join(map(wide,[alias.casefold(),e['white'],e['translucent']]))+'},')
lines+=['};','struct HonorSvgAsset{wchar_t const *name;char const *svg;};','inline constexpr HonorSvgAsset HonorSvgAssets[]={']
for p in sorted((assets/'library/TopBar').glob('*.svg'))+sorted((assets/'library/DockIcon-通透').glob('*.svg')):
    svg=p.read_text('utf-8-sig').strip()
    ET.fromstring(svg)
    # Only monochrome glyphs adapt to system ink. Preserve colored app logos/gradients.
    colors=set(re.findall(r'(?:fill|stroke)="(#[a-fA-F0-9]{6})"',svg))
    neutral=lambda c: c[1:3].lower()==c[3:5].lower()==c[5:7].lower()
    mono=all(neutral(c) for c in colors) and '<linearGradient' not in svg and '<radialGradient' not in svg
    if p.stem.startswith('Battery') or p.stem in ('havenotify', 'nonotify', 'notification'):
        svg=re.sub(r'(fill|stroke)="(#[a-fA-F0-9]{6})"',lambda m:m[1]+'="currentColor"' if neutral(m[2]) else m[0],svg)
    elif mono: svg=re.sub(r'(fill|stroke)="#[a-fA-F0-9]{6}"',r'\1="currentColor"',svg)
    # Consistent D2D viewport; viewBox and original geometry/opacity are retained.
    svg=re.sub(r'(<svg\b[^>]*\bwidth=")[^"]*(")',r'\g<1>24\2',svg,count=1)
    svg=re.sub(r'(<svg\b[^>]*\bheight=")[^"]*(")',r'\g<1>24\2',svg,count=1)
    chunks=[svg[i:i+6000] for i in range(0,len(svg),6000)]
    lines.append('    {'+wide('Honor:'+p.stem)+','+'\n'.join('R"HI('+s+')HI"' for s in chunks)+'},')
lines+=['};']
write(root/'src/composition-trial/native-honor-assets.h','\n'.join(lines)+'\n')
runtime=[]
for e in manifest['files']:
    rel=e['original']
    if rel.startswith(('DockIcon-','系统图标/','通知图标/Apps/')) and rel.endswith('.png') or rel in {'TopBar/start-menu.png','TopBar/start-menu-transparent.png'}:
        runtime.append(e['path'])
write(assets/'runtime-files.json',json.dumps(dict(schemaVersion=1,files=runtime),ensure_ascii=False,indent=2)+'\n')
import xml.sax.saxutils as xml
props=['<Project>','  <ItemGroup>']
for relative in runtime+['manifest.json','catalog.json','NOTICE.md']:
    include=xml.escape('$(MSBuildThisFileDirectory)'+relative, {'"':'&quot;'})
    link=xml.escape('honor-icons/'+relative, {'"':'&quot;'})
    props.append('    <Content Include="'+include+'" Link="'+link+'" CopyToOutputDirectory="PreserveNewest" CopyToPublishDirectory="PreserveNewest" />')
props+=['  </ItemGroup>','</Project>']
write(assets/'RuntimeAssets.props','\n'.join(props)+'\n')
print(json.dumps(dict(imported=len(manifest['files']),apps=len(app_entries),svg=len(list((assets/'library/TopBar').glob('*.svg'))),runtimePng=len(runtime),bytes=sum(e['bytes'] for e in manifest['files'])),ensure_ascii=False))
