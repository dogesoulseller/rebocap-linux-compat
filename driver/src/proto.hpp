// Minimal protobuf codec for rebocap_pb.RebocapMsg (proto/ReboTracker.proto).
// Handwritten so that the driver doesn't load libprotobuf or abseil into vrserver.
#pragma once

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace rebo {
    enum MsgKind {
        kNone = 0,
        kPosition = 1,
        kTrackerAdded = 2,
        kTrackerStatus = 3,
        kUniverseChange = 4,
        kPathFailed = 5,
        kWipInfo = 6,
        kWipSetting = 7,
        kWipFuncStateChange = 8,
    };

    enum Status { kDisconnected = 0, kOk = 1, kBusy = 2, kError = 3, kOccluded = 4 };

    struct Msg {
        int kind = kNone;
        int32_t tracker_id = 0;

        // position
        std::vector<double> q; // w, x, y, z
        std::vector<float> pos; // x, y, z in metres

        // tracker_added
        int32_t role = 0;

        std::string name;

        // tracker_status
        int32_t status = 0;

        float battery = 0; // 0..1

        // wip_setting
        bool open_wip = false;
        bool output_as_treadmill = false;
        bool use_left_hand = false;
        bool joystick_control = false;
        bool replace_hand_pose = false;

        // wip_info: x = walking speed 0..1.
        // The vendor's driver does not use y or the embedded position.
        float wip_x = 0;
        float wip_y = 0;

        int32_t wip_status = 0;
    };

    enum WipStatus { kWipStopWalking = 0, kWipStartWalking = 1, kWipStopped = 2, kWipWalking = 3, kWipExist = 4 };

    namespace detail {
        struct Reader {
            const uint8_t *p, *end;
            bool ok = true;

            bool done() const {
                return p >= end || !ok;
            }

            uint64_t varint() {
                uint64_t v = 0;
                for (int shift = 0; shift < 64 && p < end; shift += 7) {
                    const uint8_t b = *p++;

                    v |= static_cast<uint64_t>(b & 0x7f) << shift;

                    if (!(b & 0x80)) {
                        return v;
                    }
                }

                ok = false;

                return 0;
            }

            template<typename T>
            T fixed() {
                T v{};

                if (static_cast<size_t>(end - p) < sizeof(T)) {
                    ok = false;
                    return v;
                }

                std::memcpy(&v, p, sizeof(T));
                p += sizeof(T);
                return v;
            }

            Reader sub() {
                const uint64_t n = varint();
                if (!ok || n > static_cast<size_t>(end - p)) {
                    ok = false;
                    return {.p = p, .end = p};
                }

                const Reader r{.p = p, .end = p + n};
                p += n;
                return r;
            }

            void skip(int wire) {
                switch (wire) {
                    case 0:
                        varint();
                        break;
                    case 1:
                        fixed<uint64_t>();
                        break;
                    case 2:
                        sub();
                        break;
                    case 5:
                        fixed<uint32_t>();
                        break;
                    default:
                        ok = false;
                }
            }
        };

        // proto3 writers send repeated scalars packed (wire type 2). Unpacked is also accepted.
        template<typename T>
        void repeated(Reader &r, const int wire, std::vector<T> &out) {
            if (wire == 2) {
                Reader s = r.sub();
                while (!s.done()) {
                    out.push_back(s.fixed<T>());
                }

                r.ok = r.ok && s.ok;
            } else {
                out.push_back(r.fixed<T>());
            }
        }

        inline void put_varint(std::string &out, uint64_t v) {
            while (v >= 0x80) {
                out.push_back(static_cast<char>(v | 0x80));
                v >>= 7;
            }

            out.push_back(static_cast<char>(v));
        }

        template<typename T>
        void put_fixed(std::string &out, T v) {
            out.append(reinterpret_cast<const char *>(&v), sizeof v);
        }

        inline std::string wrap(const int field, const std::string &inner) {
            std::string out;
            put_varint(out, static_cast<uint64_t>(field) << 3 | 2);
            put_varint(out, inner.size());
            return out + inner;
        }
    } // namespace detail

    inline bool decode(const uint8_t *data, size_t len, Msg &m) {
        using namespace detail;

        Reader top{data, data + len};
        while (!top.done()) {
            const uint64_t tag = top.varint();
            const int field = static_cast<int>(tag >> 3);

            if (const int wire = static_cast<int>(tag & 7); wire != 2 || field < kPosition || field > kWipFuncStateChange) {
                top.skip(wire);
                continue;
            }

            Reader r = top.sub();
            m = Msg{};
            m.kind = field;

            while (!r.done()) {
                const uint64_t t = r.varint();
                const int f = static_cast<int>(t >> 3);

                if (const int w = static_cast<int>(t & 7); f == 1 && w == 0 && field <= kTrackerStatus) {
                    m.tracker_id = static_cast<int32_t>(r.varint());
                } else if (field == kPosition && f == 10) {
                    repeated(r, w, m.q);
                } else if (field == kPosition && f == 11) {
                    repeated(r, w, m.pos);
                } else if (field == kTrackerAdded && f == 3 && w == 0) {
                    m.role = static_cast<int32_t>(r.varint());
                } else if (field == kTrackerAdded && f == 4 && w == 2) {
                    const Reader s = r.sub();
                    m.name.assign(s.p, s.end);
                } else if (field == kTrackerStatus && f == 2 && w == 0) {
                    m.status = static_cast<int32_t>(r.varint());
                } else if (field == kTrackerStatus && f == 3 && w == 5) {
                    m.battery = r.fixed<float>();
                } else if (field == kWipSetting && w == 0 && f >= 1 && f <= 5) {
                    (f == 1   ? m.open_wip
                     : f == 2 ? m.output_as_treadmill
                     : f == 3 ? m.use_left_hand
                     : f == 4 ? m.joystick_control
                              : m.replace_hand_pose) = r.varint() != 0;
                } else if (field == kWipInfo && f == 2 && w == 5) {
                    m.wip_x = r.fixed<float>();
                } else if (field == kWipInfo && f == 3 && w == 5) {
                    m.wip_y = r.fixed<float>();
                } else if (field == kWipInfo && f == 4 && w == 0) {
                    m.wip_status = static_cast<int32_t>(r.varint());
                } else {
                    r.skip(w);
                }
            }

            top.ok = top.ok && r.ok;
        }

        return top.ok && m.kind != kNone;
    }

    inline std::string encode_status(const int32_t id, const int32_t status) {
        std::string in;

        if (id) {
            in.push_back(0x08);
            detail::put_varint(in, static_cast<uint32_t>(id));
        }

        if (status) {
            in.push_back(0x10);
            detail::put_varint(in, static_cast<uint32_t>(status));
        }

        return detail::wrap(kTrackerStatus, in);
    }

    inline std::string encode_position(const int32_t id, const double q_wxyz[4], const float pos[3]) {
        std::string in;

        if (id) {
            in.push_back(0x08);
            detail::put_varint(in, static_cast<uint32_t>(id));
        }

        in.push_back(0x52);
        in.push_back(32);

        for (int i = 0; i < 4; i++) {
            detail::put_fixed(in, q_wxyz[i]);
        }

        in.push_back(0x5a);
        in.push_back(12);

        for (int i = 0; i < 3; i++) {
            detail::put_fixed(in, pos[i]);
        }

        return detail::wrap(kPosition, in);
    }

    inline std::string encode_universe(const float yaw, const float pos[3]) {
        std::string in;

        if (yaw != 0.0f) {
            in.push_back(0x0d);
            detail::put_fixed(in, yaw);
        }

        in.push_back(0x12);
        in.push_back(12);

        for (int i = 0; i < 3; i++) {
            detail::put_fixed(in, pos[i]);
        }

        return detail::wrap(kUniverseChange, in);
    }

    inline std::string encode_path_failed(int32_t failed) {
        std::string in;

        if (failed) {
            in.push_back(0x08);
            detail::put_varint(in, static_cast<uint32_t>(failed));
        }

        return detail::wrap(kPathFailed, in);
    }
} // namespace rebo
