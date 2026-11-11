"""Ask the SteamVR compositor for a stereo screenshot of whatever app is rendering (the frame the headset gets, pre-distortion)."""
import openvr, sys, time, os
out = sys.argv[1] if len(sys.argv) > 1 else r'C:\Games\ssuvr-work\hmdshots\shot'
vr = openvr.init(openvr.VRApplication_Background)
try:
    ss = openvr.VRScreenshots()
    h = ss.requestScreenshot(openvr.VRScreenshotType_Stereo, out + '_preview', out + '_stereo')
    print('requested handle', h)
    for i in range(100):
        time.sleep(0.1)
        if os.path.exists(out + '_stereo.png') or os.path.exists(out + '_preview.png'):
            break
    print(sorted(f for f in os.listdir(os.path.dirname(out)) if f.startswith(os.path.basename(out))))
finally:
    openvr.shutdown()
