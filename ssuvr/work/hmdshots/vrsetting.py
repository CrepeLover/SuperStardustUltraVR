"""vrsetting.py <section> <key> [int value]: read or set a SteamVR setting live through OpenVR."""
import openvr, sys
openvr.init(openvr.VRApplication_Background)
try:
    s = openvr.VRSettings()
    sec, key = sys.argv[1], sys.argv[2]
    if len(sys.argv) > 3:
        s.setInt32(sec, key, int(sys.argv[3]))
        s.sync(True)
    try:
        print(sec, key, '=', s.getInt32(sec, key))
    except Exception as e:
        try: print(sec, key, '=', s.getFloat(sec, key))
        except Exception as e2: print('read failed', e, e2)
finally:
    openvr.shutdown()
