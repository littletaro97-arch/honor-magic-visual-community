#pragma once
// Windows delivers the initial setting and subsequent changes to the existing
// message-only relay. No timer, extra window, or repeated status polling.
namespace NativePowerPolicy {
inline constexpr GUID Legacy{0xe00958c0,0xc213,0x4ace,{0xac,0x77,0xfe,0xcc,0xed,0x2e,0xee,0xa5}};
inline constexpr GUID Energy{0x550e8400,0xe29b,0x41d4,{0xa7,0x16,0x44,0x66,0x55,0x44,0x00,0x00}};
inline bool Accept(Model &model,POWERBROADCAST_SETTING const &setting){
    bool modern=IsEqualGUID(setting.PowerSetting,Energy);if(!modern&&(!IsEqualGUID(setting.PowerSetting,Legacy)||model.saverModern))return false;
    if(setting.DataLength!=sizeof(DWORD))return false;DWORD value=0;memcpy(&value,setting.Data,sizeof(value));if(value>(modern?2u:1u))return false;
    bool changed=!model.saverKnown||model.saverActive!=(value!=0);model.saverKnown=true;model.saverActive=value!=0;model.saverModern|=modern;return changed;
}
}
inline void App::SubscribePower(){
    for(auto id:{NativePowerPolicy::Legacy,NativePowerPolicy::Energy})if(auto token=RegisterPowerSettingNotification(relay,&id,DEVICE_NOTIFY_WINDOW_HANDLE))powerNotifications.push_back(token);
}
inline void App::RefreshRenderPolicy(){
    ClearRetiringPopups();
    if(shell&&shell->captionClose)shell->captionClose->Reset();
    for(auto const &window:windows)if(window->hwnd){if(window->batch)window->AnimationDone(window->animationGeneration);window->ReleaseSurfaces();window->Draw();}
    auto refresh=[&](NativePopup &p){if(!p.visible)return;if(p.dismissing){p.FinishHide();return;}p.FinishResize();p.FinishFlyoutResize();p.interactionVisual.Release(p.popupRoot);p.ResetPanelMotion();if(p.visual){p.visual.StopAnimation(L"Opacity");p.visual.Opacity(1);}p.Draw();};
    if(popup)refresh(*popup);
    if(shell&&shell->toast)refresh(*shell->toast);
    if(menus)for(auto &level:menus->levels)refresh(*level.popup);
    if(shell)for(auto const &[id,bar]:shell->bars){bar->barInteraction.Release(bar->toolbarRoot);if(bar->hidingToolbar)bar->FinishToolbarHide();bar->ResetDockBodyMotion();if(bar->dock){if(!bar->edge)bar->PositionBar(SWP_NOACTIVATE);bar->ClearDockHover();}else{bar->lastPage.clear();bar->pageAlpha=100;KillTimer(bar->hwnd,96);}bar->toolbarBackgroundKey.clear();bar->Draw();}
}
