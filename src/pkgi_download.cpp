extern "C" {
#include "pkgi.h"
#include "pkgi_utils.h"
}
#include "pkgi_download.hpp"

#include <boost/scope_exit.hpp>

#include <stddef.h>

// clang-format off
static const uint8_t pkg_psp_key[] = { 0x07, 0xf2, 0xc6, 0x82, 0x90, 0xb5, 0x0d, 0x2c, 0x33, 0x81, 0x8d, 0x70, 0x9b, 0x60, 0xe6, 0x2b };
static const uint8_t pkg_vita_2[] = { 0xe3, 0x1a, 0x70, 0xc9, 0xce, 0x1d, 0xd7, 0x2b, 0xf3, 0xc0, 0x62, 0x29, 0x63, 0xf2, 0xec, 0xcb };
static const uint8_t pkg_vita_3[] = { 0x42, 0x3a, 0xca, 0x3a, 0x2b, 0xd5, 0x64, 0x9f, 0x96, 0x86, 0xab, 0xad, 0x6f, 0xd8, 0x80, 0x1f };
static const uint8_t pkg_vita_4[] = { 0xaf, 0x07, 0xfd, 0x59, 0x65, 0x25, 0x27, 0xba, 0xf1, 0x33, 0x89, 0x66, 0x8b, 0x17, 0xd9, 0xea };
// clang-format on

Download::Download(std::unique_ptr<Http> http) : _http(std::move(http))
{
}

void Download::download_start(void)
{
    LOG("resuming pkg download from %llu offset", download_offset);
    info_update = pkgi_time_msec() + 1000;
    update_status("Downloading");
}

void Download::update_progress()
{
    uint32_t info_now = pkgi_time_msec();
    if (info_now >= info_update)
    {
        update_progress_cb(*this);
        info_update = info_now + 500;
    }
}

int Download::download_data(
        uint8_t* buffer, uint32_t size, int encrypted, int save)
{
    if (is_canceled())
    {
        return 0;
    }

    update_progress();

    if (!*_http)
    {
        LOG("requesting %s @ %llu", download_url, download_offset);
        _http->start(download_url, download_offset);

        const int64_t http_length = _http->get_length();
        if (http_length < 0)
        {
            throw DownloadError("HTTP返回長度未知");
        }

        download_size = http_length + download_offset;

        LOG("http response length = %lld, total pkg size = %llu",
            http_length,
            download_size);
        info_start = pkgi_time_msec();
        info_update = pkgi_time_msec() + 500;
    }

    int read = _http->read(buffer, size);
    if (read < 0)
    {
        char error[256];
        pkgi_snprintf(error, sizeof(error), "HTTP下載錯誤 0x%08x", read);
        throw DownloadError(error);
    }
    else if (read == 0)
    {
        throw DownloadError("HTTP 鏈接關閉");
    }
    download_offset += read;

    sha256_update(&sha, buffer, read);

    if (encrypted)
    {
        aes128_ctr(&aes, iv, encrypted_base + encrypted_offset, buffer, read);
        encrypted_offset += read;
    }

    if (save)
    {
        uint32_t write;
        if (encrypted)
        {
            write = (uint32_t)min64(decrypted_size, read);
            decrypted_size -= write;
        }
        else
        {
            write = read;
        }

        if (!pkgi_write(item_file, buffer, write))
        {
            char error[256];
            pkgi_snprintf(
                    error, sizeof(error), "無法寫入到 %s", item_path);
            throw DownloadError(error);
        }
    }

    return read;
}

// this includes creating of all the parent folders necessary to actually
// create file
int Download::create_file(void)
{
    char folder[256];
    pkgi_strncpy(folder, sizeof(folder), item_path);
    char* last = pkgi_strrchr(folder, '/');
    *last = 0;

    if (!pkgi_mkdirs(folder))
    {
        char error[256];
        pkgi_snprintf(error, sizeof(error), "無法創建文件夾 %s", folder);
        throw DownloadError(error);
    }

    LOG("creating %s file", item_name);
    item_file = pkgi_create(item_path);
    if (!item_file)
    {
        char error[256];
        pkgi_snprintf(error, sizeof(error), "無法創建文件 %s", item_name);
        throw DownloadError(error);
    }

    return 1;
}

int Download::download_head(const uint8_t* rif)
{
    LOG("downloading pkg head");

    pkgi_strncpy(item_name, sizeof(item_name), "Preparing...");
    pkgi_snprintf(
            item_path, sizeof(item_path), "%s/sce_sys/package/head.bin", root);

    BOOST_SCOPE_EXIT_ALL(&)
    {
        if (item_file)
        {
            pkgi_close(item_file);
            item_file = NULL;
        }
    };

    if (!create_file())
    {
        char error[256];
        pkgi_snprintf(error, sizeof(error), "無法創建 %s", item_path);
        throw DownloadError(error);
    }

    head_size = PKG_HEADER_SIZE + PKG_HEADER_EXT_SIZE;
    uint32_t head_offset = 0;
    while (head_offset != head_size)
    {
        int size = download_data(
                head + head_offset, head_size - head_offset, 0, 1);
        if (size <= 0)
        {
            return 0;
        }
        head_offset += size;
    }

    if (get32be(head) != 0x7f504b47 ||
        get32be(head + PKG_HEADER_SIZE) != 0x7F657874)
    {
        throw DownloadError("錯誤的pkg文件頭");
    }

    if (rif && !pkgi_memequ(rif + 0x10, head + 0x30, 0x30))
    {
        throw DownloadError("zRIF 內容ID與pkg文件不匹配");
    }

    meta_offset = get32be(head + 8);
    meta_count = get32be(head + 12);
    index_count = get32be(head + 20);
    total_size = get64be(head + 24);
    enc_offset = get64be(head + 32);
    enc_size = get64be(head + 40);
    LOG("meta_offset=%u meta_count=%u index_count=%u total_size=%llu "
        "enc_offset=%llu enc_size=%llu",
        meta_offset,
        meta_count,
        index_count,
        total_size,
        enc_offset,
        enc_size);

    if (enc_offset > sizeof(head))
    {
        LOG("pkg file head is too large");
        throw DownloadError("pkg 不被支持, head.bin過大");
    }

    pkgi_memcpy(iv, head + 0x70, sizeof(iv));

    uint8_t key[AES_BLOCK_SIZE];
    int key_type = head[0xe7] & 7;
    if (key_type == 1)
    {
        pkgi_memcpy(key, pkg_psp_key, sizeof(key));
    }
    else if (key_type == 2)
    {
        aes128_ctx ctx;
        aes128_init(&ctx, pkg_vita_2);
        aes128_encrypt(&ctx, iv, key);
    }
    else if (key_type == 3)
    {
        aes128_ctx ctx;
        aes128_init(&ctx, pkg_vita_3);
        aes128_encrypt(&ctx, iv, key);
    }
    else if (key_type == 4)
    {
        aes128_ctx ctx;
        aes128_init(&ctx, pkg_vita_4);
        aes128_encrypt(&ctx, iv, key);
    }
    else
        throw DownloadError("無效的秘鑰類型 " + std::to_string(key_type));

    aes128_ctr_init(&aes, key);

    uint32_t target_size = (uint32_t)enc_offset;
    while (head_size != target_size)
    {
        int size =
                download_data(head + head_size, target_size - head_size, 0, 1);
        if (size <= 0)
        {
            return 0;
        }
        head_size += size;
    }

    index_size = 0;

    uint32_t offset = meta_offset;
    for (uint32_t i = 0; i < meta_count; i++)
    {
        if (offset + 16 >= enc_offset)
        {
            throw DownloadError("pkg文件過小或損壞");
        }

        uint32_t type = get32be(head + offset + 0);
        uint32_t size = get32be(head + offset + 4);

        if (type == 2)
        {
            content_type = get32be(head + offset + 8);
            // 6 PSX game
            // 21 PSV game (or update)
            // 22 PSV DLC
            if (content_type != 21 && content_type != 22 && content_type != 6)
            {
                throw DownloadError(
                        "不支持的包類型: " +
                        std::to_string(content_type));
            }
        }
        else if (type == 13)
        {
            // index_offset = get32be(head + offset + 8);
            index_size = get32be(head + offset + 12);
        }
        offset += 8 + size;
    }

    target_size = (uint32_t)(enc_offset + index_count * 32);
    if (target_size > sizeof(head))
    {
        LOG("pkg file head is too large");
        throw DownloadError("pkg 不被支持, head.bin過大");
    }

    while (head_size != target_size)
    {
        int size =
                download_data(head + head_size, target_size - head_size, 0, 1);
        if (size <= 0)
        {
            return 0;
        }
        head_size += size;
    }

    uint64_t item_offset;
    {
        uint8_t item[32];
        pkgi_memcpy(item, head + enc_offset, sizeof(item));
        aes128_ctr(&aes, iv, 0, item, sizeof(item));

        item_offset = get64be(item + 8);
    }

    if (index_size && item_offset != index_size)
    {
        throw DownloadError(
                "聲明錯誤, 文件預讀不匹配, 希望獲得: " +
                std::to_string(index_size) +
                ", 但是獲得: " + std::to_string(item_offset));
    }

    target_size = (uint32_t)(enc_offset + item_offset);
    if (target_size > sizeof(head))
    {
        LOG("pkg file head is too large");
        throw DownloadError("pkg 不被支持, head.bin過大");
    }

    while (head_size != target_size)
    {
        int size =
                download_data(head + head_size, target_size - head_size, 0, 1);
        if (size <= 0)
        {
            return 0;
        }
        head_size += size;
    }

    LOG("head.bin downloaded");
    return 1;
}

int Download::download_files(void)
{
    LOG("downloading encrypted files");

    BOOST_SCOPE_EXIT_ALL(&)
    {
        if (item_file)
        {
            pkgi_close(item_file);
            item_file = NULL;
        }
    };

    for (uint32_t index = 0; index < index_count; index++)
    {
        uint8_t item[32];
        pkgi_memcpy(
                item, head + enc_offset + sizeof(item) * index, sizeof(item));
        aes128_ctr(&aes, iv, sizeof(item) * index, item, sizeof(item));

        uint32_t name_offset = get32be(item + 0);
        uint32_t name_size = get32be(item + 4);
        uint64_t item_offset = get64be(item + 8);
        uint64_t item_size = get64be(item + 16);
        uint8_t type = item[27];

        if (name_size > sizeof(item_name) - 1 ||
            enc_offset + name_offset + name_size > total_size)
        {
            throw DownloadError("pkg文件過小或損壞");
        }

        pkgi_memcpy(item_name, head + enc_offset + name_offset, name_size);
        aes128_ctr(&aes, iv, name_offset, (uint8_t*)item_name, name_size);
        item_name[name_size] = 0;

        uint64_t encrypted_size = (item_size + AES_BLOCK_SIZE - 1) &
                                  ~((uint64_t)AES_BLOCK_SIZE - 1);
        decrypted_size = item_size;
        encrypted_base = item_offset;
        encrypted_offset = 0;
        item_index = index;

        LOG("[%u/%u] %s item_offset=%llu item_size=%llu type=%u",
            item_index + 1,
            index_count,
            item_name,
            item_offset,
            item_size,
            type);

        if (content_type == 6)
        {
            if (std::string(item_name) == "USRDIR/CONTENT/DOCUMENT.DAT")
                pkgi_snprintf(
                        item_path, sizeof(item_path), "%s/DOCUMENT.DAT", root);
            else if (std::string(item_name) == "USRDIR/CONTENT/EBOOT.PBP")
                pkgi_snprintf(
                        item_path, sizeof(item_path), "%s/EBOOT.PBP", root);
            else
            {
                while (encrypted_offset != encrypted_size)
                {
                    uint32_t read = (uint32_t)min64(
                            sizeof(down), encrypted_size - encrypted_offset);
                    int size = download_data(down, read, 1, 0);
                    if (size <= 0)
                    {
                        return 0;
                    }
                }
                continue;
            }
        }
        else
            pkgi_snprintf(
                    item_path, sizeof(item_path), "%s/%s", root, item_name);

        if (type == 4)
        {
            pkgi_mkdirs(item_path);
            continue;
        }
        else if (type == 18)
        {
            continue;
        }

        if (!create_file())
        {
            char error[256];
            pkgi_snprintf(error, sizeof(error), "cannot create %s", item_name);
            throw DownloadError(error);
        }

        if (enc_offset + item_offset + encrypted_offset != download_offset)
        {
            throw DownloadError(
                    "pkg文件不被支援, 文件順序錯誤, "
                    "期望的: " +
                    std::to_string(
                            enc_offset + item_offset + encrypted_offset) +
                    ", 實際的: " + std::to_string(download_offset));
        }

        if (enc_offset + item_offset + item_size > total_size)
        {
            throw DownloadError("pkg文件過小或損壞");
        }

        while (encrypted_offset != encrypted_size)
        {
            uint32_t read = (uint32_t)min64(
                    sizeof(down), encrypted_size - encrypted_offset);
            int size = download_data(down, read, 1, 1);
            if (size <= 0)
            {
                return 0;
            }
        }

        pkgi_close(item_file);
        item_file = NULL;
    }

    item_index = -1;

    LOG("all files decrypted");
    return 1;
}

int Download::download_tail(void)
{
    LOG("downloading tail.bin");

    BOOST_SCOPE_EXIT_ALL(&)
    {
        if (item_file)
        {
            pkgi_close(item_file);
            item_file = NULL;
        }
    };

    pkgi_strncpy(item_name, sizeof(item_name), "Finishing...");
    pkgi_snprintf(
            item_path, sizeof(item_path), "%s/sce_sys/package/tail.bin", root);

    if (!create_file())
    {
        char error[256];
        pkgi_snprintf(error, sizeof(error), "無法創建 %s", item_path);
        throw DownloadError(error);
    }

    uint64_t tail_offset = enc_offset + enc_size;
    while (download_offset < tail_offset)
    {
        uint32_t read =
                (uint32_t)min64(sizeof(down), tail_offset - download_offset);
        int size = download_data(down, read, 0, 0);
        if (size <= 0)
        {
            return 0;
        }
    }

    while (download_offset != total_size)
    {
        uint32_t read =
                (uint32_t)min64(sizeof(down), total_size - download_offset);
        int size = download_data(down, read, 0, content_type != 6);
        if (size <= 0)
        {
            return 0;
        }
    }

    LOG("tail.bin downloaded");
    return 1;
}

int Download::check_integrity(const uint8_t* digest)
{
    if (!digest)
    {
        LOG("no integrity provided, skipping check");
        return 1;
    }

    uint8_t check[SHA256_DIGEST_SIZE];
    sha256_finish(&sha, check);

    LOG("checking integrity of pkg");
    if (!pkgi_memequ(digest, check, SHA256_DIGEST_SIZE))
    {
        LOG("pkg integrity is wrong, removing head.bin & resume data");

        char path[256];
        pkgi_snprintf(path, sizeof(path), "%s/sce_sys/package/head.bin", root);
        pkgi_rm(path);

        throw DownloadError("pkg完整性效驗錯誤, 請嘗試重新下載");
    }

    LOG("pkg integrity check succeeded");
    return 1;
}

int Download::create_stat()
{
    LOG("creating stat.bin");
    update_status("Creating stat.bin");

    char path[256];
    pkgi_snprintf(path, sizeof(path), "%s/sce_sys/package/stat.bin", root);

    uint8_t stat[768] = {0};
    if (!pkgi_save(path, stat, sizeof(stat)))
    {
        char error[256];
        pkgi_snprintf(error, sizeof(error), "無法將rif保存到 %s", path);
        throw DownloadError(error);
    }

    LOG("stat.bin created");
    return 1;
}

int Download::create_rif(const uint8_t* rif)
{
    LOG("creating work.bin");
    update_status("Creating work.bin");

    char path[256];
    pkgi_snprintf(path, sizeof(path), "%s/sce_sys/package/work.bin", root);

    if (!pkgi_save(path, rif, PKGI_RIF_SIZE))
    {
        char error[256];
        pkgi_snprintf(error, sizeof(error), "無法將rif保存到 %s", path);
        throw DownloadError(error);
    }

    LOG("work.bin created");
    return 1;
}

int Download::pkgi_download(
        const char* content,
        const char* url,
        const uint8_t* rif,
        const uint8_t* digest)
{
    pkgi_snprintf(root, sizeof(root), "%s/%s", pkgi_get_temp_folder(), content);
    LOG("temp installation folder: %s", root);

    update_status("Downloading");
    sha256_init(&sha);

    item_file = NULL;
    item_index = -1;
    download_size = 0;
    download_offset = 0;
    download_content = content;
    download_url = url;

    info_start = pkgi_time_msec();
    info_update = info_start + 1000;

    if (!download_head(rif))
        return 0;
    if (!download_files())
        return 0;
    if (!download_tail())
        return 0;
    if (content_type != 6)
    {
        if (!create_stat())
            return 0;
    }
    if (!check_integrity(digest))
        return 0;
    if (rif)
    {
        if (!create_rif(rif))
            return 0;
    }

    return 1;
}
