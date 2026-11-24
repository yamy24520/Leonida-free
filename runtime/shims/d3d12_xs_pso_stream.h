// Xbox derived pipeline states (device slot 55) over PC pipeline state streams.
//
// Slot 86 creates a PSO from a D3D12_PIPELINE_STATE_STREAM_DESC. The proxy keeps a deep copy of that stream on the
// PSO it returns (ID3D12Object private data, so the copy is freed with the PSO). Slot 55 takes a base PSO and up to
// three state records; it patches a copy of the base stream and creates the new PSO through the same path.
//
// Record layout (TomBox's create_derived_pso_x, GPL-3.0): 0x14c bytes, a u32 tag, then the state payload at +4.
//   tag 0: D3D12_DEPTH_STENCIL_DESC (0x34) -> the DEPTH_STENCIL or DEPTH_STENCIL1 subobject (first 0x34 bytes)
//   tag 1: D3D12_RASTERIZER_DESC (0x2c)
//   tag 2: D3D12_BLEND_DESC (0x148)
//   tag 3: sample mask (u32)
//   tag 5: the StencilReadMask and StencilWriteMask bytes (payload +0x10, +0x11) overwrite those two bytes of the base's
//          DEPTH_STENCIL / DEPTH_STENCIL1; the title writes no other byte of it (read from the retail build's record writes)
// Any other tag is refused: its meaning is not known.
#pragma once

#include <d3d12.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace rt::d3d12_xs {

inline constexpr std::size_t kDerivedRecordSize = 0x14c;
inline constexpr std::size_t kDerivedRecordPayload = 4;
// The console passes the count of records it built: raster, depth-stencil, the tag-5 record and blend can all be
// present, so the count reaches 4 (measured from the title's record writes, see the notes in docs/d3d12_vulkan.md).
inline constexpr unsigned kDerivedMaxRecords = 4;

// An owned deep copy of one pipeline state stream. The root signature references are held until destruction; shader
// bytecode, stream-output and input-layout arrays and strings, and cached blobs are copied, and their pointers in the
// copy point at the copies.
class PsoStreamRecord {
public:
    // Null when the stream is malformed, holds a subobject type this module does not walk, or is too large.
    static std::unique_ptr<PsoStreamRecord> capture(const D3D12_PIPELINE_STATE_STREAM_DESC& desc);
    ~PsoStreamRecord();
    PsoStreamRecord(const PsoStreamRecord&) = delete;
    PsoStreamRecord& operator=(const PsoStreamRecord&) = delete;

    // The stream bytes. Its pointers refer to this record's copies, so the record must outlive any use of the desc.
    const std::vector<unsigned char>& bytes() const { return bytes_; }
    // The PC runtime reads the stream and never writes it; the const_cast only meets the desc's void* member.
    D3D12_PIPELINE_STATE_STREAM_DESC desc() const {
        D3D12_PIPELINE_STATE_STREAM_DESC d{};
        d.SizeInBytes = bytes_.size();
        d.pPipelineStateSubobjectStream = const_cast<unsigned char*>(bytes_.data());
        return d;
    }

private:
    PsoStreamRecord() = default;
    bool copy_pointed_data(std::size_t type, std::size_t payload_offset);

    std::vector<unsigned char> bytes_;
    std::vector<std::vector<unsigned char>> blobs_;  // owned copies; their buffers do not move when this grows
    std::vector<ID3D12RootSignature*> roots_;        // AddRef'd
};

enum class DeriveStatus { ok, unknown_tag, missing_state, malformed };

// Writes the base stream with `count` records applied (count 1..kDerivedMaxRecords; `records` holds count records).
// The result's pointers still refer to the base record's copies.
DeriveStatus derive_stream(const PsoStreamRecord& base, const unsigned char* records, unsigned count,
                           std::vector<unsigned char>& out);

// Keeps `record` on `object` (lifetime follows the object) and finds it again. attach returns false on failure, and
// then the record is dropped. find returns null when no record was attached to that object.
bool attach_pso_record(ID3D12Object* object, std::unique_ptr<PsoStreamRecord> record);
const PsoStreamRecord* find_pso_record(ID3D12Object* object);

}  // namespace rt::d3d12_xs
