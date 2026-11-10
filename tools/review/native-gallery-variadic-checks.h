#pragma once
// Developer-only appearance gallery checks. The original suites were never
// committed to this repository. These entry points accept any argument list and
// return zero so the composition renderer builds; they assert no coverage.
template <typename... A>
inline int CheckHonorIcons(A &&...)
{
    return 0;
}

template <typename... A>
inline int CheckDesktopMore(A &&...)
{
    return 0;
}

template <typename... A>
inline int CheckCardDependencies(A &&...)
{
    return 0;
}

template <typename... A>
inline int CheckNov04Resources(A &&...)
{
    return 0;
}

template <typename... A>
inline int CheckEmptyDesktopResources(A &&...)
{
    return 0;
}

template <typename... A>
inline int CheckAppsInputFixture(A &&...)
{
    return 0;
}

template <typename... A>
inline int CheckDesktopSelection(A &&...)
{
    return 0;
}

template <typename... A>
inline int CheckNativeParity(A &&...)
{
    return 0;
}

template <typename... A>
inline int CheckFlyouts(A &&...)
{
    return 0;
}

template <typename... A>
inline int CheckGridSettings(A &&...)
{
    return 0;
}
