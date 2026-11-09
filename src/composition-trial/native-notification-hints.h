#pragma once
#include <string_view>
namespace NativeNotificationHints {
inline bool SessionStatus(bool access,std::wstring_view previous,std::wstring_view next){
    // Missing/revoked notification access is a settings status, not an
    // unsolicited toast. The explicit authorization entry remains in the panel.
    return access&&!next.empty()&&next!=previous;
}
}
