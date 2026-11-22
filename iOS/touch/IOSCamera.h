/* UT2004 has no CAMERA: the DOOM port's IOSTouch.m calls these, and its CAMERA
   buttons are hidden (IOSTouch_Unused), so they do nothing. */
#ifndef UT2004_IOSCAMERA_H
#define UT2004_IOSCAMERA_H
static inline void IOSCamera_Photo(void) {}
static inline void IOSCamera_Selfie(void) {}
static inline void IOSCamera_Burst(void) {}
static inline void IOSCamera_Freeze(void) {}
static inline void IOSCamera_Cinematic(void) {}
static inline int IOSCamera_IsBusy(void) { return 0; }
static inline int IOSCamera_Stage(void) { return 0; }
static inline void IOSCamera_ShowOptions(void) {}
#endif
