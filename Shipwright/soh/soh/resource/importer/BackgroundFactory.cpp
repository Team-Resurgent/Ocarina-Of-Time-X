#include "soh/resource/importer/BackgroundFactory.h"
#include "soh/resource/type/Background.h"

namespace SOH {
std::shared_ptr<Ship::IResource>
ResourceFactoryBinaryBackgroundV0::ReadResource(std::shared_ptr<Ship::File> file,
                                                std::shared_ptr<Ship::ResourceInitData> initData) {
    if (!FileHasValidFormatAndReader(file, initData)) {
        return nullptr;
    }

    auto background = std::make_shared<Background>(initData);
    auto reader = std::get<std::shared_ptr<Ship::BinaryReader>>(file->Reader);

    uint32_t dataSize = reader->ReadUInt32();

    background->Data.reserve(dataSize);

    for (uint32_t i = 0; i < dataSize; i++) {
        background->Data.push_back(reader->ReadUByte());
    }
#ifdef LUS_XBOX
    // z_room.c's swapAndConvertJPEG decodes the JPEG in place, writing the 320x240 16-bit picture (150 KB) over this,
    // and the renderer then reads that much from here: room for it, or both run past the JPEG's own few dozen KB
    // (heap corruption, then a read of an unmapped page; the warp test's crashes in the Market and the Bazaar)
    if (background->Data.size() < 320 * 240 * 2) {
        background->Data.resize(320 * 240 * 2);
    }
#endif

    return background;
}
} // namespace SOH
