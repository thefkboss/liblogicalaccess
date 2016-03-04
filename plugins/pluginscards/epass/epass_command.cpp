#include "epass_command.hpp"
#include "epass_readercardadapter.hpp"
#include <cassert>
#include <iomanip>
#include <logicalaccess/bufferhelper.hpp>
#include <logicalaccess/crypto/sha.hpp>
#include <readercardadapters/iso7816readercardadapter.hpp>

using namespace logicalaccess;

EPassCommand::EPassCommand()
{
}

static ByteVector get_challenge(std::shared_ptr<ISO7816ReaderCardAdapter> rca)
{
    std::vector<unsigned char> result;

    result = rca->sendAPDUCommand(0, 0x84, 0x00, 0x00, 8);
    EXCEPTION_ASSERT_WITH_LOG(result.size() >= 2, LibLogicalAccessException,
                              "GetChallenge reponse is too short.");
    if (result[result.size() - 2] == 0x90 && result[result.size() - 1] == 0x00)
    {
        return std::vector<unsigned char>(result.begin(), result.end() - 2);
    }
    return result;
}

bool EPassCommand::authenticate(const std::string &mrz)
{
    crypto_ = std::make_shared<EPassCrypto>(mrz);
    cryptoChanged();

    std::shared_ptr<ISO7816ReaderCardAdapter> rcu =
        std::dynamic_pointer_cast<ISO7816ReaderCardAdapter>(getReaderCardAdapter());
    assert(rcu);
    auto challenge = get_challenge(rcu);
    std::cout << "Challenge: " << challenge << std::endl;
    auto tmp = crypto_->step1(challenge);

    tmp = rcu->sendAPDUCommand(0x00, 0x82, 0x00, 0x00, 0x28, tmp, 0x28);
    // drop status bytes.
    EXCEPTION_ASSERT_WITH_LOG(tmp.size() == 40, LibLogicalAccessException,
                              "Unexpected response length");
    std::cout << "bla = " << tmp << std::endl;

    bool ret = crypto_->step2(tmp);
    std::cout << "Ret = " << ret << std::endl;

    return ret;
}

EPassEFCOM EPassCommand::readEFCOM()
{
    ByteVector ef_com_raw;
    selectEF({0x01, 0x1E});

    auto data = readEF(1, 1);
    EXCEPTION_ASSERT_WITH_LOG(data.size() >= 10, LibLogicalAccessException,
                              "EF.COM data seems too short");
    EXCEPTION_ASSERT_WITH_LOG(data[0] == 0x60, LibLogicalAccessException,
                              "Invalid Tag for EF.COM file.");
    return EPassUtils::parse_ef_com(data);
}

bool EPassCommand::selectApplication(const ByteVector &app_id)
{
    std::shared_ptr<ISO7816ReaderCardAdapter> rca =
        std::dynamic_pointer_cast<ISO7816ReaderCardAdapter>(getReaderCardAdapter());
    assert(rca);
    auto ret = rca->sendAPDUCommand(0x00, 0xA4, 0x04, 0x0C, app_id.size(), app_id);
    return true;
}

bool EPassCommand::selectEF(const ByteVector &file_id)
{
    std::shared_ptr<ISO7816ReaderCardAdapter> rca =
        std::dynamic_pointer_cast<ISO7816ReaderCardAdapter>(getReaderCardAdapter());
    assert(rca);
    auto ret = rca->sendAPDUCommand(0x00, 0xA4, 0x02, 0x0C, file_id.size(), file_id);
    std::cout << "CLEAR: " << ret << std::endl;
    return true;
}

bool EPassCommand::selectIssuerApplication()
{
    return selectApplication({0xA0, 0, 0, 0x02, 0x47, 0x10, 0x01});
}

void EPassCommand::setReaderCardAdapter(std::shared_ptr<ReaderCardAdapter> adapter)
{
    Commands::setReaderCardAdapter(adapter);
    auto epass_rca = std::dynamic_pointer_cast<EPassReaderCardAdapter>(adapter);
    if (epass_rca)
        epass_rca->setEPassCrypto(crypto_);
}

void EPassCommand::cryptoChanged()
{
    auto epass_rca =
        std::dynamic_pointer_cast<EPassReaderCardAdapter>(getReaderCardAdapter());
    if (epass_rca)
        epass_rca->setEPassCrypto(crypto_);
}

ByteVector EPassCommand::readBinary(uint16_t offset, uint8_t length)
{
    uint8_t p1 = 0;
    uint8_t p2 = 0;
    p1         = static_cast<uint8_t>(0x7f & (offset >> 8));
    p2         = static_cast<uint8_t>(offset & 0xFF);

    std::shared_ptr<ISO7816ReaderCardAdapter> rca =
        std::dynamic_pointer_cast<ISO7816ReaderCardAdapter>(getReaderCardAdapter());
    if (length)
        return rca->sendAPDUCommand(0x00, 0xB0, p1, p2, length);
    else
        return rca->sendAPDUCommand(0x00, 0xB0, p1, p2);
}

EPassDG1 EPassCommand::readDG1()
{
    selectEF({0x01, 0x01});
    auto raw = readEF(1, 1);
    std::cout << "DG1: " << raw << std::endl;
    return EPassUtils::parse_dg1(raw);
}

EPassDG2 EPassCommand::readDG2()
{
    selectEF({0x01, 0x02});
    std::cout << "AFTER SELECT FILE" << std::endl;

    // File tag is 2 bytes and size is 2 bytes too.
    auto dg2_raw = readEF(2, 2);

    auto dg2 = EPassUtils::parse_dg2(dg2_raw);
    return dg2;
}

ByteVector EPassCommand::readEF(uint8_t size_bytes, uint8_t size_offset)
{
    ByteVector ef_raw;
    uint8_t initial_read_len = static_cast<uint8_t>(size_bytes + size_offset);

    auto data = readBinary(0, initial_read_len);
    EXCEPTION_ASSERT_WITH_LOG(data.size() == initial_read_len,
                              LibLogicalAccessException, "Wrong data size.");
    ef_raw.insert(ef_raw.end(), data.begin(), data.end());

    // compute the length of the file, based on the number of bytes representing the
    // size
    // and the initial offset of those bytes.
    uint16_t length = 0;
    for (int i = 0; i < size_bytes; ++i)
        length |= data[size_offset + i] << (size_bytes - i - 1) * 8;

    uint16_t offset = initial_read_len;
    while (length)
    {
        // somehow reading more will cause invalid checksum.
        // todo: investigate.
        uint8_t to_read = static_cast<uint8_t>(length > 100 ? 100 : length);
        data            = readBinary(offset, to_read);
        EXCEPTION_ASSERT_WITH_LOG(data.size() == to_read, LibLogicalAccessException,
                                  "Wrong data size");
        ef_raw.insert(ef_raw.end(), data.begin(), data.end());
        offset += data.size();
        length -= data.size();
    }
    return ef_raw;
}

void EPassCommand::readSOD()
{
    // SOD is ASN.1
    // For now we are not able to use it.
    auto hash_1 = compute_hash({1, 1});
    auto hash_2 = compute_hash({1, 2});

    std::cout << "DG1 Hash: " << BufferHelper::getHex(hash_1) << std::endl;
    std::cout << "DG2 Hash: " << BufferHelper::getHex(hash_2) << std::endl;

    std::cout << "selecting SOD" << std::endl;
    selectEF({0x01, 0x1D});
    std::cout << "reading SOD" << std::endl;
    auto tmp = readEF(2, 2);

    std::cout << "read: " << tmp << std::endl;
    std::ofstream of("/tmp/lama", std::ios::binary | std::ios::trunc);
    of.write((const char *)tmp.data() + 4, tmp.size() - 4);
}

ByteVector EPassCommand::compute_hash(const ByteVector &file_id)
{
    selectEF(file_id);

    ByteVector content;
    if (file_id == ByteVector{1, 1})
        content = readEF(1, 1);
    else if (file_id == ByteVector{1, 2})
        content = readEF(2, 2);

    // Hash algorithm can vary.
    return openssl::SHA1Hash(content);
    // return openssl::SHA256Hash(content);
}
