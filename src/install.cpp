#include "install.hpp"

#include "extractzip.hpp"
#include "file.hpp"
#include "log.hpp"
#include "sfo.hpp"
#include "sqlite.hpp"

#include <boost/scope_exit.hpp>

#include <psp2/io/fcntl.h>
#include <psp2/promoterutil.h>

std::vector<std::string> pkgi_get_installed_games()
{
    return pkgi_list_dir_contents("ux0:app");
}

namespace
{
std::string pkgi_extract_package_version(const std::string& package)
{
    const auto sfo = pkgi_load(fmt::format("{}/sce_sys/param.sfo", package));
    return pkgi_sfo_get_string(sfo.data(), sfo.size(), "APP_VER");
}
}

std::string pkgi_get_game_version(const std::string& titleid)
{
    const auto patch_dir = fmt::format("ux0:patch/{}", titleid);
    if (pkgi_file_exists(patch_dir.c_str()))
        return pkgi_extract_package_version(patch_dir);

    const auto game_dir = fmt::format("ux0:app/{}", titleid);
    if (pkgi_file_exists(game_dir.c_str()))
        return pkgi_extract_package_version(game_dir);

    return "";
}

bool pkgi_dlc_is_installed(const char* content)
{
    return pkgi_file_exists(
            fmt::format("ux0:addcont/{:.9}/{:.16}", content + 7, content + 20)
                    .c_str());
}

bool pkgi_psm_is_installed(const char* titleid)
{
    return pkgi_file_exists(fmt::format("ux0:psm/{}", titleid).c_str());
}

bool pkgi_psp_is_installed(const char* psppartition, const char* content)
{
    return pkgi_file_exists(
                   fmt::format(
                           "{}pspemu/ISO/{:.9}.iso", psppartition, content + 7)
                           .c_str()) ||
           pkgi_file_exists(
                   fmt::format(
                           "{}pspemu/PSP/GAME/{:.9}", psppartition, content + 7)
                           .c_str());
}

bool pkgi_psx_is_installed(const char* psppartition, const char* content)
{
    return pkgi_file_exists(
            fmt::format("{}pspemu/PSP/GAME/{:.9}", psppartition, content + 7)
                    .c_str());
}

void pkgi_install(const char* contentid)
{
    char path[128];
    snprintf(path, sizeof(path), "ux0:pkgj/%s", contentid);

    LOG("calling scePromoterUtilityPromotePkgWithRif on %s", path);
    const auto res = scePromoterUtilityPromotePkgWithRif(path, 1);
    if (res < 0)
        throw formatEx<std::runtime_error>(
                "調用NoNpDrm函數錯誤: {:#08x}\n{}",
                static_cast<uint32_t>(res),
                static_cast<uint32_t>(res) == 0x80870004
                        ? "請檢查NoNpDrm插件安裝是否正確"
                        : "");
}

void pkgi_install_update(const char* contentid)
{
    pkgi_mkdirs("ux0:patch");

    const auto titleid = fmt::format("{:.9}", contentid + 7);
    const auto src = fmt::format("ux0:pkgj/{}", contentid);
    const auto dest = fmt::format("ux0:patch/{}", titleid);

    LOGF("deleting previous patch at {}", dest);
    pkgi_delete_dir(dest);

    LOGF("installing update from {} to {}", src, dest);
    const auto res = sceIoRename(src.c_str(), dest.c_str());
    if (res < 0)
        throw formatEx<std::runtime_error>(
                "無法重命名: {:#08x}", static_cast<uint32_t>(res));

    const auto sfo = pkgi_load(fmt::format("{}/sce_sys/param.sfo", dest));
    const auto version = pkgi_sfo_get_string(sfo.data(), sfo.size(), "APP_VER");

    LOGF("found version is {}", version);
    if (version.empty())
        throw std::runtime_error("在param.sfo中無法獲取版本參數");
    if (version.size() != 5)
        throw formatEx<std::runtime_error>(
                "版本參數不正確: {}", version.size());

    SqlitePtr _sqliteDb;
    sqlite3* raw_appdb;
    SQLITE_CHECK(
            sqlite3_open("ur0:shell/db/app.db", &raw_appdb),
            "can't open app.db database");
    _sqliteDb.reset(raw_appdb);

    sqlite3_stmt* stmt;
    SQLITE_CHECK(
            sqlite3_prepare_v2(
                    _sqliteDb.get(),
                    R"(UPDATE tbl_appinfo
                    SET val = ?
                    WHERE titleId = ? AND key = 3168212510)",
                    -1,
                    &stmt,
                    nullptr),
            "can't prepare version update SQL statement");
    BOOST_SCOPE_EXIT_ALL(&)
    {
        sqlite3_finalize(stmt);
    };

    sqlite3_bind_text(stmt, 1, version.data(), version.size(), nullptr);
    sqlite3_bind_text(stmt, 2, titleid.data(), titleid.size(), nullptr);

    const auto err = sqlite3_step(stmt);
    if (err != SQLITE_DONE)
        throw formatEx<std::runtime_error>(
                "無法執行版本更新的SQL語句:\n{}",
                sqlite3_errmsg(_sqliteDb.get()));
}

void pkgi_install_comppack(
        const std::string& titleid, bool patch, const std::string& version)
{
    const auto src = fmt::format("ux0:pkgj/{}-comp.ppk", titleid);
    const auto dest = fmt::format("ux0:rePatch/{}", titleid);

    if (!patch)
        pkgi_delete_dir(dest);

    pkgi_mkdirs(dest.c_str());

    LOGF("installing comp pack from {} to {}", src, dest);
    pkgi_extract_zip(src, dest);

    pkgi_save(
            fmt::format(
                    "{}/{}_comppack_version", dest, patch ? "patch" : "base"),
            version.data(),
            version.size());
}

CompPackVersion pkgi_get_comppack_versions(const std::string& titleid)
{
    const auto dir = fmt::format("ux0:rePatch/{}", titleid);

    const auto present = pkgi_file_exists(dir.c_str());

    const auto base = [&] {
        try
        {
            const auto data = pkgi_load(dir + "/base_comppack_version");
            return std::string(data.begin(), data.end());
        }
        catch (const std::exception& e)
        {
            LOGF("no base comppack version: {}", e.what());
            // return empty string if not found
            return std::string{};
        }
    }();
    const auto patch = [&] {
        try
        {
            const auto data = pkgi_load(dir + "/patch_comppack_version");
            return std::string(data.begin(), data.end());
        }
        catch (const std::exception& e)
        {
            LOGF("no patch comppack version: {}", e.what());
            // return empty string if not found
            return std::string{};
        }
    }();

    return {present, base, patch};
}

void pkgi_install_psmgame(const char* contentid)
{
    pkgi_mkdirs("ux0:psm");
    const auto titleid = fmt::format("{:.9}", contentid + 7);
    const auto src = fmt::format("ux0:pkgj/{}", contentid);
    const auto dest = fmt::format("ux0:psm/{}", titleid);

    LOGF("installing psm game from {} to {}", src, dest);
    const auto res = sceIoRename(src.c_str(), dest.c_str());
    if (res < 0)
        throw formatEx<std::runtime_error>(
                "無法重命名: {:#08x}", static_cast<uint32_t>(res));
}

void pkgi_install_pspgame(const char* partition, const char* contentid)
{
    LOG("Installing a PSP/PSX game");
    const auto path = fmt::format("{}pkgj/{}", partition, contentid);
    const auto dest =
            fmt::format("{}pspemu/PSP/GAME/{:.9}", partition, contentid + 7);

    pkgi_mkdirs(fmt::format("{}pspemu/PSP/GAME", partition).c_str());

    LOG("installing psx game at %s to %s", path.c_str(), dest.c_str());
    int res = sceIoRename(path.c_str(), dest.c_str());
    if (res < 0)
        throw std::runtime_error(fmt::format(
                "無法重命名: {:#08x}", static_cast<uint32_t>(res)));
}

void pkgi_install_pspgame_as_iso(const char* partition, const char* contentid)
{
    const auto path = fmt::format("{}pkgj/{}", partition, contentid);
    const auto dest =
            fmt::format("{}pspemu/PSP/GAME/{:.9}", partition, contentid + 7);

    // this is actually a misnamed ISO file
    const auto eboot = fmt::format("{}/EBOOT.PBP", path);
    const auto content = fmt::format("{}/CONTENT.DAT", path);
    const auto pspkey = fmt::format("{}/PSP-KEY.EDAT", path);
    const auto isodest =
            fmt::format("{}pspemu/ISO/{:.9}.iso", partition, contentid + 7);

    pkgi_mkdirs(fmt::format("{}pspemu/ISO", partition).c_str());

    LOG("installing psp game at %s to %s", path.c_str(), dest.c_str());
    pkgi_rename(eboot.c_str(), isodest.c_str());

    const auto content_exists = pkgi_file_exists(content.c_str());
    const auto pspkey_exists = pkgi_file_exists(pspkey.c_str());
    if (content_exists || pspkey_exists)
        pkgi_mkdirs(dest.c_str());

    if (content_exists)
        pkgi_rename(
                content.c_str(), fmt::format("{}/CONTENT.DAT", dest).c_str());
    if (pspkey_exists)
        pkgi_rename(
                pspkey.c_str(), fmt::format("{}/PSP-KEY.EDAT", dest).c_str());

    pkgi_delete_dir(path);
}
