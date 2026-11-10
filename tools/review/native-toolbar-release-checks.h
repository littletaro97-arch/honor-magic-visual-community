#pragma once
// Developer-only appearance gallery check. The original suite was never
// committed to this repository; these entry points stay as verified no-ops so
// the composition renderer keeps building without inventing coverage.
template <typename App>
inline int CheckToolbarRelease(App &)
{
    return 0;
}
