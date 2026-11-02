// Black Arrow Online launcher.
//
// Starts xemu from the "system" folder next to this program. When no hard
// disk has been set up yet it first sets up a new console (xemu then makes
// the XBL Dashboard its dashboard). Nothing outside the package folder is
// ever changed.
//
// It also looks for a newer build on the releases page and offers to install
// it. An update replaces the programs only, never the console's files.
//
// Build: csc /target:winexe /win32icon:xemu-icon.ico
//            /r:System.IO.Compression.dll /r:System.IO.Compression.FileSystem.dll
//            Launcher.cs
using System;
using System.Collections.Generic;
using System.ComponentModel;
using System.Diagnostics;
using System.Drawing;
using System.Drawing.Drawing2D;
using System.Drawing.Imaging;
using System.IO;
using System.IO.Compression;
using System.Linq;
using System.Net;
using System.Runtime.InteropServices;
using System.Security.Cryptography;
using System.Text.RegularExpressions;
using System.Threading;
using System.Windows.Forms;

static class Launcher
{
    const string BundledBios = "Complex_4627v1.03.bin";
    const string BundledMcpx = "mcpx.bin";

    static string hddTarget, eepromTarget, toml;

    public static string systemDir;

    [DllImport("user32.dll")]
    static extern bool SetProcessDPIAware();

    // Lets the emulator bring its window to the front, which Windows only
    // allows a program that the foreground program hands it to
    [DllImport("user32.dll")]
    static extern bool AllowSetForegroundWindow(int processId);
    const int ASFW_ANY = -1;

    [STAThread]
    static int Main(string[] args)
    {
        systemDir = Path.Combine(AppDomain.CurrentDomain.BaseDirectory, "system");
        hddTarget = Path.Combine(systemDir, "xbox_hdd.qcow2");
        eepromTarget = Path.Combine(systemDir, "eeprom.bin");
        toml = Path.Combine(systemDir, "xemu.toml");

        SetProcessDPIAware();
        Application.EnableVisualStyles();
        Application.SetCompatibleTextRenderingDefault(false);

        if (!File.Exists(Path.Combine(systemDir, "xemu.exe"))) {
            MessageBox.Show("The \"system\" folder is missing or incomplete.\n\n" +
                            "Unzip the whole package and start it from there.",
                            "Black Arrow Online", MessageBoxButtons.OK,
                            MessageBoxIcon.Warning);
            return 1;
        }

        EnsureSettings();

        // Unattended modes, used for testing
        string mode = Arg(args, "--mode"), preview = Arg(args, "--preview");
        string previewUpdate = Arg(args, "--preview-update");
        bool noLaunch = args.Contains("--nolaunch");

        Updater.CleanUp();

        // Help > Versions & Updates in xemu: install the chosen build (newer
        // or older) once xemu has closed
        string installBuild = Arg(args, "--install-build");
        if (installBuild != null) {
            int pid;
            int build = UpdateInfo.CodeFromText(installBuild);
            int.TryParse(Arg(args, "--wait-pid") ?? "", out pid);
            using (var form = new InstallForm(build, pid)) {
                Application.Run(form);
                AllowSetForegroundWindow(ASFW_ANY);
                // The installed build's own launcher starts xemu; after a
                // cancel, the build that was there
                Process.Start(new ProcessStartInfo {
                    FileName = Application.ExecutablePath,
                    Arguments = "--noupdate",
                    UseShellExecute = false
                });
            }
            return 0;
        }

        if (previewUpdate != null) {
            using (var form = new UpdateForm(Updater.Sample(), previewUpdate)) {
                Application.Run(form);
            }
            return 0;
        }
        if (mode == null && preview == null && !args.Contains("--noupdate")) {
            UpdateInfo update = Updater.Check();
            if (update != null) {
                using (var form = new UpdateForm(update, null)) {
                    Application.Run(form);
                    if (form.Installed) {
                        // Carry on in the new launcher
                        AllowSetForegroundWindow(ASFW_ANY);
                        Process.Start(new ProcessStartInfo {
                            FileName = Application.ExecutablePath,
                            Arguments = "--noupdate",
                            UseShellExecute = false
                        });
                        return 0;
                    }
                }
            }
        }
        try {
            if (mode == "content") {
                InstallContent(Arg(args, "--pack"), null);
                return 0;
            } else if (mode == "update") {
                UpdateInfo update = Updater.Check();
                if (update == null) {
                    Console.Error.WriteLine("No update.");
                    return 2;
                }
                Updater.Install(update, delegate { });
                return 0;
            } else if (mode == "clean") {
                StartClean(null);
            } else if (mode == "import") {
                ImportExisting(Arg(args, "--hdd"), Arg(args, "--eeprom"),
                               Arg(args, "--bios"), Arg(args, "--mcpx"), null);
            }
        } catch (Exception e) {
            Console.Error.WriteLine(e.Message);
            return 1;
        }

        if (preview != null || !File.Exists(hddTarget)) {
            using (var form = new SetupForm(preview)) {
                Application.Run(form);
                if (!form.Completed) {
                    return 0;
                }
            }
        }
        if (!noLaunch) {
            AllowSetForegroundWindow(ASFW_ANY);
            Process.Start(new ProcessStartInfo {
                FileName = Path.Combine(systemDir, "xemu.exe"),
                WorkingDirectory = systemDir,
                UseShellExecute = false
            });
        }
        return 0;
    }

    static string Arg(string[] args, string name)
    {
        int i = Array.IndexOf(args, name);
        return i >= 0 && i + 1 < args.Length ? args[i + 1] : null;
    }

    // --- setup actions ---

    // --- content pack ---
    //
    // The Xbox BIOS, boot ROM, clean hard disk and dashboard discs are not
    // part of the builds published on the releases page. They come in a
    // separate zip, the content pack, laid out like the package folder.

    public const string ContentPackName = "BlackArrowOnline-content.zip";
    const string ContentMarker = "system/content.txt";

    const string DefaultSettings =
        "[general]\n" +
        "show_welcome = false\n" +
        "skip_boot_anim = true\n" +
        "games_dir = '..\\games'\n" +
        "\n" +
        "[general.updates]\n" +
        "check = false\n" +
        "\n" +
        "[display.quality]\n" +
        "surface_scale = 2\n" +
        "skip_reuse_copyback = true\n" +
        "\n" +
        "[net]\n" +
        "enable = true\n" +
        "dns_redirect = 'xb_online'\n" +
        "\n" +
        "[net.nat]\n" +
        "forward_ports = [\n" +
        "    { host = 3074, guest = 3074, protocol = 'udp'}\n" +
        "    ]\n" +
        "\n" +
        "[sys]\n" +
        "mem_limit = '128'\n" +
        "\n" +
        "[sys.files]\n" +
        "bootrom_path = '" + BundledMcpx + "'\n" +
        "flashrom_path = '" + BundledBios + "'\n" +
        "eeprom_path = 'eeprom.bin'\n" +
        "hdd_path = 'xbox_hdd.qcow2'\n";

    // A folder made from a release alone has no settings or games folder yet
    static void EnsureSettings()
    {
        try {
            if (!File.Exists(toml)) {
                File.WriteAllText(toml, DefaultSettings);
            }
            Directory.CreateDirectory(GamesDir);
        } catch (IOException) {
        } catch (UnauthorizedAccessException) {
        }
    }

    static bool HasBundled(string name)
    {
        return File.Exists(Path.Combine(systemDir, name));
    }

    // Whether a clean start is impossible until the content pack is added
    public static bool ContentMissing
    {
        get {
            return !HasBundled(BundledBios) || !HasBundled(BundledMcpx) ||
                   !File.Exists(Path.Combine(systemDir, "fresh", "xbox_hdd.qcow2"));
        }
    }

    public static string DownloadsDir
    {
        get {
            return Path.Combine(
                Environment.GetFolderPath(Environment.SpecialFolder.UserProfile), "Downloads");
        }
    }

    // A content pack next to this program or in Downloads, newest first
    public static string FindContentPack()
    {
        foreach (string dir in new[] { AppDomain.CurrentDomain.BaseDirectory, DownloadsDir }) {
            try {
                string found = Directory.GetFiles(dir, "BlackArrowOnline-content*.zip")
                    .OrderByDescending(f => File.GetLastWriteTimeUtc(f)).FirstOrDefault();
                if (found != null) {
                    return found;
                }
            } catch (IOException) {
            } catch (UnauthorizedAccessException) {
            }
        }
        return null;
    }

    public static void InstallContent(string zip, Action<int> progress)
    {
        if (string.IsNullOrEmpty(zip) || !File.Exists(zip)) {
            throw new InvalidOperationException("The content pack was not found.");
        }
        string root = AppDomain.CurrentDomain.BaseDirectory;
        string system = Path.GetFullPath(systemDir).TrimEnd(Path.DirectorySeparatorChar);
        // The console's own files and the programs do not come from a pack
        string[] keep = { "xemu.toml", "eeprom.bin", "xbox_hdd.qcow2", "xemu.exe" };

        ZipArchive archive;
        try {
            archive = ZipFile.OpenRead(zip);
        } catch (InvalidDataException) {
            throw new InvalidOperationException(
                "That file is not a zip, or it did not download completely.");
        }
        using (archive) {
            if (!archive.Entries.Any(e => e.FullName.Replace('\\', '/').Equals(
                    ContentMarker, StringComparison.OrdinalIgnoreCase))) {
                throw new InvalidOperationException(
                    "That is not a Black Arrow Online content pack.\n\n" +
                    "It is a zip named " + ContentPackName + ".");
            }
            long total = Math.Max(archive.Entries.Sum(e => e.Length), 1), done = 0;
            var buffer = new byte[4 * 1024 * 1024];
            foreach (ZipArchiveEntry entry in archive.Entries) {
                string name = entry.FullName.Replace('\\', '/');
                if (name.EndsWith("/")) {
                    continue;
                }
                string target = Path.GetFullPath(Path.Combine(root, name));
                string folder = Path.GetDirectoryName(target);
                // Only into the system folder, whatever the zip says
                if (!(folder + Path.DirectorySeparatorChar).StartsWith(
                        system + Path.DirectorySeparatorChar, StringComparison.OrdinalIgnoreCase) ||
                    (string.Equals(folder, system, StringComparison.OrdinalIgnoreCase) &&
                     keep.Contains(Path.GetFileName(target), StringComparer.OrdinalIgnoreCase))) {
                    done += entry.Length;
                    continue;
                }
                Directory.CreateDirectory(folder);
                using (Stream src = entry.Open())
                using (var dst = new FileStream(target, FileMode.Create, FileAccess.Write)) {
                    int n;
                    while ((n = src.Read(buffer, 0, buffer.Length)) > 0) {
                        dst.Write(buffer, 0, n);
                        done += n;
                        if (progress != null) {
                            progress((int)Math.Min(100, done * 100 / total));
                        }
                    }
                }
            }
        }
    }

    public static void StartClean(Action<int> progress)
    {
        if (ContentMissing) {
            throw new InvalidOperationException("Add the content pack first.");
        }
        CopyFile(Path.Combine(systemDir, "fresh", "xbox_hdd.qcow2"), hddTarget, progress);
        File.Delete(eepromTarget);
        SetTomlPath("flashrom_path", BundledBios);
        SetTomlPath("bootrom_path", BundledMcpx);
        ClearFirstRunState();
    }

    public static void ImportExisting(string hdd, string eeprom, string bios,
                                      string mcpx, Action<int> progress)
    {
        Check(hdd, "hard disk", true, n => n >= 1024 * 1024,
              "That is too small to be an Xbox hard disk image. " +
              "It is usually named xbox_hdd.qcow2.");
        Check(eeprom, "EEPROM", true, n => n == 256,
              "An Xbox EEPROM file is exactly 256 bytes. It is usually named eeprom.bin.");
        Check(bios, "BIOS", false, n => n == 256 * 1024 || n == 1024 * 1024,
              "An Xbox BIOS file is 256 KB or 1 MB.");
        Check(mcpx, "MCPX boot ROM", false, n => n == 512,
              "An MCPX boot ROM file is exactly 512 bytes.");
        if ((string.IsNullOrEmpty(bios) && !HasBundled(BundledBios)) ||
            (string.IsNullOrEmpty(mcpx) && !HasBundled(BundledMcpx))) {
            throw new InvalidOperationException(
                "This folder has no BIOS and MCPX boot ROM yet.\n\n" +
                "Choose your own BIOS and MCPX files, or add the content pack first.");
        }

        CopyFile(hdd, hddTarget, progress);
        File.Copy(eeprom, eepromTarget, true);
        if (!string.IsNullOrEmpty(bios)) {
            File.Copy(bios, Path.Combine(systemDir, "imported_bios.bin"), true);
            SetTomlPath("flashrom_path", "imported_bios.bin");
        }
        if (!string.IsNullOrEmpty(mcpx)) {
            File.Copy(mcpx, Path.Combine(systemDir, "imported_mcpx.bin"), true);
            SetTomlPath("bootrom_path", "imported_mcpx.bin");
        }
        ClearFirstRunState();
    }

    static void Check(string path, string what, bool required,
                      Func<long, bool> sizeOk, string sizeHelp)
    {
        if (string.IsNullOrEmpty(path)) {
            if (required) {
                throw new InvalidOperationException("Choose your " + what + " file first.");
            }
            return;
        }
        if (!File.Exists(path)) {
            throw new InvalidOperationException("The " + what + " file was not found:\n" + path);
        }
        if (!sizeOk(new FileInfo(path).Length)) {
            throw new InvalidOperationException(
                "That does not look like the right " + what + " file.\n\n" + sizeHelp);
        }
    }

    public static void RemovePartialHdd()
    {
        try { File.Delete(hddTarget); } catch (IOException) { }
    }

    static void CopyFile(string from, string to, Action<int> progress)
    {
        var buffer = new byte[4 * 1024 * 1024];
        using (var src = new FileStream(from, FileMode.Open, FileAccess.Read,
                                        FileShare.ReadWrite))
        using (var dst = new FileStream(to, FileMode.Create, FileAccess.Write)) {
            long total = Math.Max(src.Length, 1), done = 0;
            int n;
            while ((n = src.Read(buffer, 0, buffer.Length)) > 0) {
                dst.Write(buffer, 0, n);
                done += n;
                if (progress != null) {
                    progress((int)(done * 100 / total));
                }
            }
        }
    }

    static void SetTomlPath(string key, string value)
    {
        string text = File.ReadAllText(toml);
        text = Regex.Replace(text, "(?m)^" + key + " = '.*'\r?$", key + " = '" + value + "'");
        File.WriteAllText(toml, text);
    }

    // Forget which disc was last used and that account setup was offered, so
    // a newly set up console is treated as new
    static void ClearFirstRunState()
    {
        var lines = File.ReadAllLines(toml).Where(
            l => !Regex.IsMatch(l, "^(xb_online_setup_offered|xbl_dash_install_tries|dvd_path) = "));
        File.WriteAllLines(toml, lines);
    }

    // Path used by an xemu already installed on this PC, to pre-fill the form
    public static string ExistingPath(string key)
    {
        try {
            string config = Path.Combine(
                Environment.GetFolderPath(Environment.SpecialFolder.ApplicationData),
                "xemu", "xemu", "xemu.toml");
            foreach (string line in File.ReadAllLines(config)) {
                Match m = Regex.Match(line, "^" + key + " = '(.*)'");
                if (m.Success && File.Exists(m.Groups[1].Value)) {
                    return m.Groups[1].Value;
                }
            }
        } catch (IOException) {
        } catch (UnauthorizedAccessException) {
        }
        return "";
    }

    public static string GamesDir
    {
        get { return Path.Combine(AppDomain.CurrentDomain.BaseDirectory, "games"); }
    }

    // Name of the first disc image in the games folder, or null
    public static string FindGame()
    {
        try {
            return Directory.GetFiles(GamesDir)
                .Where(f => f.EndsWith(".iso", StringComparison.OrdinalIgnoreCase) ||
                            f.EndsWith(".xiso", StringComparison.OrdinalIgnoreCase))
                .Select(f => Path.GetFileName(f)).FirstOrDefault();
        } catch (IOException) {
            return null;
        }
    }

    // Our logo, built into the launcher (csc /resource:...,bao_logo.png)
    public static Image Logo()
    {
        try {
            var stream = typeof(Launcher).Assembly
                             .GetManifestResourceStream("bao_logo.png");
            return stream == null ? null : Image.FromStream(stream);
        } catch (Exception) {
            return null;
        }
    }

    public static Icon AppIcon()
    {
        try {
            return Icon.ExtractAssociatedIcon(Application.ExecutablePath);
        } catch (Exception) {
            return null;
        }
    }
}

// A build is a whole number (26), or a sub build of it (26.1, 26.2...) that
// updates only some systems. version.txt keeps "build=26", which launchers
// before 26 read, and adds "sub=1"; the release tag is build-26.1. Code holds
// both as one number, Build * 100 + Sub (2601 for 26.1), for comparing.
class UpdateInfo
{
    public int Build, Sub;
    public string Xemu = "", Date = "", Sha256 = "", Notes = "";
    // Where its files are; empty for the newest build
    public string Url = "";

    public int Code { get { return Build > 0 ? Build * 100 + Sub : 0; } }
    // "26" or "26.1"
    public string Text { get { return CodeText(Code); } }

    public static string CodeText(int code)
    {
        return code % 100 != 0 ? (code / 100) + "." + (code % 100) : (code / 100).ToString();
    }

    // From "26" or "26.1"; 0 when it is neither
    public static int CodeFromText(string text)
    {
        string[] parts = (text ?? "").Trim().Split('.');
        int build, sub = 0;
        if (parts.Length > 2 || !int.TryParse(parts[0], out build) || build <= 0 ||
            (parts.Length == 2 && (!int.TryParse(parts[1], out sub) || sub < 0 || sub > 99))) {
            return 0;
        }
        return build * 100 + sub;
    }
}

// Looks for, downloads and installs newer builds from the releases page
static class Updater
{
    public const string ReleasesPage =
        "https://github.com/Rubyxm88/Black-Arrow-Online/releases";
    const string PackageName = "BlackArrowOnline-update.zip";
    const int CheckTimeoutMs = 5000;

    // Files that belong to the player's console and are never replaced
    static readonly string[] Keep = {
        "xemu.toml", "eeprom.bin", "xbox_hdd.qcow2"
    };

    static string BaseUrl
    {
        get {
            // BAO_UPDATE_URL points the check somewhere else, for testing
            string url = Environment.GetEnvironmentVariable("BAO_UPDATE_URL");
            if (string.IsNullOrEmpty(url)) {
                url = ReleasesPage + "/latest/download/";
            }
            return url.EndsWith("/") ? url : url + "/";
        }
    }

    static string InSystem(string name)
    {
        return Path.Combine(Launcher.systemDir, name);
    }

    static UpdateInfo Parse(string text)
    {
        var info = new UpdateInfo();
        foreach (string line in text.Split('\n')) {
            int eq = line.IndexOf('=');
            if (eq < 1) {
                continue;
            }
            string key = line.Substring(0, eq).Trim(), value = line.Substring(eq + 1).Trim();
            if (key == "build") {
                int.TryParse(value, out info.Build);
            } else if (key == "sub") {
                int.TryParse(value, out info.Sub);
                if (info.Sub < 0 || info.Sub > 99) {
                    info.Sub = 0;
                }
            } else if (key == "xemu") {
                info.Xemu = value;
            } else if (key == "date") {
                info.Date = value;
            } else if (key == "sha256") {
                info.Sha256 = value.ToLowerInvariant();
            } else if (key == "notes") {
                info.Notes = value;
            }
        }
        return info;
    }

    // What is installed here; build 0 when the package predates updates
    public static UpdateInfo Installed()
    {
        try {
            return Parse(File.ReadAllText(InSystem("version.txt")));
        } catch (IOException) {
            return new UpdateInfo();
        }
    }

    public static UpdateInfo Sample()
    {
        return Parse("build=12\nxemu=0.8.140\ndate=2026-11-02\n" +
                     "notes=Follows xemu 0.8.140.");
    }

    static WebRequest Request(string url, int timeoutMs)
    {
        // TLS 1.2, which older .NET versions do not offer by default
        ServicePointManager.SecurityProtocol |= (SecurityProtocolType)3072;
        WebRequest request = WebRequest.Create(url);
        request.Timeout = timeoutMs;
        var http = request as HttpWebRequest;
        if (http != null) {
            http.UserAgent = "BlackArrowOnline-Launcher";
            http.ReadWriteTimeout = timeoutMs;
        }
        return request;
    }

    // The newer build on the releases page, or null when there is none, the
    // player skipped it, checking is turned off or there is no connection
    public static UpdateInfo Check()
    {
        if (File.Exists(InSystem("no-update-check.txt"))) {
            return null;
        }
        UpdateInfo latest = null;
        var thread = new Thread(delegate() {
            try {
                using (WebResponse response = Request(BaseUrl + "version.txt", CheckTimeoutMs).GetResponse())
                using (var reader = new StreamReader(response.GetResponseStream())) {
                    char[] buffer = new char[4096];
                    int n = reader.ReadBlock(buffer, 0, buffer.Length);
                    latest = Parse(new string(buffer, 0, n));
                }
            } catch (Exception) {
                // Offline or no release yet: start as usual
            }
        });
        thread.IsBackground = true;
        thread.Start();
        if (!thread.Join(CheckTimeoutMs) || latest == null) {
            return null;
        }

        int skipped = 0;
        try {
            skipped = UpdateInfo.CodeFromText(File.ReadAllText(InSystem("update-skip.txt")));
        } catch (IOException) {
        }
        return latest.Code > Installed().Code && latest.Code != skipped ? latest : null;
    }

    // A build by its number (UpdateInfo.Code), newer or older than the
    // installed one, as published on the releases page
    public static UpdateInfo ForBuild(int code)
    {
        string build = UpdateInfo.CodeText(code);
        string url = ReleasesPage + "/download/build-" + build + "/";
        UpdateInfo info;
        try {
            using (WebResponse response = Request(url + "version.txt", 15000).GetResponse())
            using (var reader = new StreamReader(response.GetResponseStream())) {
                info = Parse(reader.ReadToEnd());
            }
        } catch (WebException e) {
            var http = e.Response as HttpWebResponse;
            if (http != null && http.StatusCode == HttpStatusCode.NotFound) {
                throw new InvalidOperationException(
                    "Build " + build + " is not on the releases page.");
            }
            throw new InvalidOperationException(
                "Could not reach the releases page: " + e.Message);
        }
        if (info.Code != code || info.Sha256.Length != 64) {
            throw new InvalidOperationException(
                "The release of build " + build + " has no usable version.txt.");
        }
        info.Url = url;
        return info;
    }

    public static void Skip(UpdateInfo update)
    {
        try {
            File.WriteAllText(InSystem("update-skip.txt"), update.Text);
        } catch (IOException) {
        }
    }

    // Remove what an earlier update left behind
    public static void CleanUp()
    {
        try {
            File.Delete(Application.ExecutablePath + ".old");
            File.Delete(InSystem("update.zip"));
            if (Directory.Exists(InSystem("update"))) {
                Directory.Delete(InSystem("update"), true);
            }
        } catch (IOException) {
        } catch (UnauthorizedAccessException) {
        }
    }

    public static void Install(UpdateInfo update, Action<int> progress)
    {
        string zip = InSystem("update.zip"), folder = InSystem("update");

        string from = update.Url.Length > 0 ? update.Url : BaseUrl;
        using (WebResponse response = Request(from + PackageName, 30000).GetResponse())
        using (Stream src = response.GetResponseStream())
        using (var dst = new FileStream(zip, FileMode.Create, FileAccess.Write)) {
            long total = Math.Max(response.ContentLength, 1), done = 0;
            var buffer = new byte[256 * 1024];
            int n;
            while ((n = src.Read(buffer, 0, buffer.Length)) > 0) {
                dst.Write(buffer, 0, n);
                done += n;
                progress((int)Math.Min(99, done * 100 / total));
            }
        }
        progress(100);

        if (update.Sha256.Length > 0) {
            string actual;
            using (var sha = SHA256.Create())
            using (var stream = File.OpenRead(zip)) {
                actual = BitConverter.ToString(sha.ComputeHash(stream)).Replace("-", "")
                                     .ToLowerInvariant();
            }
            if (actual != update.Sha256) {
                File.Delete(zip);
                throw new InvalidOperationException(
                    "The download did not arrive complete. Try again later.");
            }
        }

        if (Directory.Exists(folder)) {
            Directory.Delete(folder, true);
        }
        ZipFile.ExtractToDirectory(zip, folder);

        string root = AppDomain.CurrentDomain.BaseDirectory;
        string self = Path.GetFullPath(Application.ExecutablePath);
        string versionFile = Path.GetFullPath(InSystem("version.txt"));
        var files = Directory.GetFiles(folder, "*", SearchOption.AllDirectories)
            .Select(f => f.Substring(folder.Length).TrimStart(Path.DirectorySeparatorChar))
            // version.txt goes last, so a failed update is offered again
            .OrderBy(f => Path.GetFullPath(Path.Combine(root, f)) == versionFile).ToList();

        foreach (string name in files) {
            string target = Path.GetFullPath(Path.Combine(root, name));
            if (Keep.Contains(Path.GetFileName(target), StringComparer.OrdinalIgnoreCase) ||
                name.StartsWith("games", StringComparison.OrdinalIgnoreCase)) {
                continue;
            }
            Directory.CreateDirectory(Path.GetDirectoryName(target));
            if (string.Equals(target, self, StringComparison.OrdinalIgnoreCase)) {
                // A running program cannot be overwritten, but it can be renamed
                File.Delete(self + ".old");
                File.Move(self, self + ".old");
            } else if (Path.GetFileName(target).Equals("xemu.exe", StringComparison.OrdinalIgnoreCase) &&
                       File.Exists(target)) {
                File.Copy(target, InSystem("xemu.previous.exe"), true);
            }
            File.Copy(Path.Combine(folder, name), target, true);
        }

        File.Delete(zip);
        Directory.Delete(folder, true);
    }

}

static class Theme
{
    public static readonly Color Back = Color.FromArgb(24, 26, 27);
    public static readonly Color PanelBack = Color.FromArgb(36, 39, 41);
    public static readonly Color Green = Color.FromArgb(60, 170, 70);
    public static readonly Color Grey = Color.FromArgb(62, 66, 69);
    public static readonly Color TextColor = Color.FromArgb(235, 235, 235);
    public static readonly Color Dim = Color.FromArgb(165, 170, 172);

    public static Label AddLabel(Control parent, string text, int x, int y, int w, int h,
                                 float size, bool bold, Color color)
    {
        var label = new Label();
        label.Text = text;
        label.SetBounds(x, y, w, h);
        label.Font = new Font("Segoe UI", size, bold ? FontStyle.Bold : FontStyle.Regular);
        label.ForeColor = color;
        label.BackColor = Color.Transparent;
        label.UseMnemonic = false;
        parent.Controls.Add(label);
        return label;
    }

    public static Button AddButton(Control parent, string text, int x, int y, int w, int h,
                                   bool primary)
    {
        var button = new Button();
        button.Text = text;
        button.SetBounds(x, y, w, h);
        button.FlatStyle = FlatStyle.Flat;
        button.FlatAppearance.BorderSize = 0;
        button.BackColor = primary ? Green : Grey;
        button.FlatAppearance.MouseOverBackColor =
            primary ? Color.FromArgb(76, 192, 86) : Color.FromArgb(78, 83, 87);
        button.ForeColor = Color.White;
        button.Font = new Font("Segoe UI", primary ? 10F : 9F, FontStyle.Bold);
        button.Cursor = Cursors.Hand;
        parent.Controls.Add(button);
        return button;
    }

    public static void SavePreview(Form form, string path)
    {
        form.Refresh();
        using (var bitmap = new Bitmap(form.Width, form.Height)) {
            form.DrawToBitmap(bitmap, new Rectangle(0, 0, form.Width, form.Height));
            bitmap.Save(path, ImageFormat.Png);
        }
    }
}

// Offers a newer build and installs it
class UpdateForm : Form
{
    public bool Installed;

    readonly UpdateInfo update;
    readonly string preview;
    readonly Button updateButton, laterButton, skipButton;
    readonly ProgressBar progress;
    readonly Label status;

    public UpdateForm(UpdateInfo info, string previewPath)
    {
        update = info;
        preview = previewPath;
        Text = "Black Arrow Online - Update";
        AutoScaleDimensions = new SizeF(96F, 96F);
        AutoScaleMode = AutoScaleMode.Dpi;
        ClientSize = new Size(560, 322);
        StartPosition = FormStartPosition.CenterScreen;
        FormBorderStyle = FormBorderStyle.FixedSingle;
        MaximizeBox = false;
        MinimizeBox = false;
        BackColor = Theme.Back;
        ForeColor = Theme.TextColor;
        Font = new Font("Segoe UI", 10F);
        Icon = Launcher.AppIcon();

        UpdateInfo installed = Updater.Installed();
        Theme.AddLabel(this, "An update is available", 24, 16, 512, 38, 18F, true,
                       Theme.TextColor);

        var panel = new Panel();
        panel.SetBounds(24, 66, 512, 132);
        panel.BackColor = Theme.PanelBack;
        Controls.Add(panel);
        Theme.AddLabel(panel, "Build " + update.Text +
                              (update.Date.Length > 0 ? "  -  " + update.Date : ""),
                       18, 12, 476, 26, 12F, true, Theme.Green);
        Theme.AddLabel(panel, (update.Xemu.Length > 0 ? "Based on xemu " + update.Xemu + ".  " : "") +
                              "You have " +
                              (installed.Build > 0 ? "build " + installed.Text : "an earlier build") + ".",
                       18, 42, 476, 22, 9F, false, Theme.Dim);
        Theme.AddLabel(panel, (update.Notes.Length > 0 ? update.Notes + "\n" : "") +
                              "Your console, Xbox Live sign-in, saves, settings and games " +
                              "are not touched.",
                       18, 68, 476, 56, 9F, false, Theme.TextColor);

        updateButton = Theme.AddButton(this, "Update now", 24, 216, 160, 42, true);
        laterButton = Theme.AddButton(this, "Later", 196, 216, 110, 42, false);
        skipButton = Theme.AddButton(this, "Skip this build", 318, 216, 140, 42, false);
        updateButton.Click += delegate { Run(); };
        laterButton.Click += delegate { Close(); };
        skipButton.Click += delegate { Updater.Skip(update); Close(); };

        var link = new LinkLabel();
        link.Text = "What changed";
        link.SetBounds(24, 276, 120, 20);
        link.Font = new Font("Segoe UI", 9F);
        link.LinkColor = Theme.Dim;
        link.ActiveLinkColor = Theme.TextColor;
        link.LinkClicked += delegate { Process.Start(Updater.ReleasesPage); };
        Controls.Add(link);

        status = Theme.AddLabel(this, "", 150, 276, 386, 20, 9F, false, Theme.Dim);
        status.TextAlign = ContentAlignment.TopRight;
        progress = new ProgressBar();
        progress.SetBounds(24, 304, 512, 8);
        progress.Style = ProgressBarStyle.Continuous;
        progress.Visible = false;
        Controls.Add(progress);
    }

    protected override void OnShown(EventArgs e)
    {
        base.OnShown(e);
        if (preview != null) {
            Theme.SavePreview(this, preview);
            Close();
        }
    }

    void Run()
    {
        updateButton.Enabled = laterButton.Enabled = skipButton.Enabled = false;
        progress.Value = 0;
        progress.Visible = true;
        status.Text = "Downloading...";

        var worker = new BackgroundWorker();
        worker.WorkerReportsProgress = true;
        worker.DoWork += delegate { Updater.Install(update, p => worker.ReportProgress(p)); };
        worker.ProgressChanged += delegate(object s, ProgressChangedEventArgs e) {
            progress.Value = Math.Min(100, e.ProgressPercentage);
            status.Text = e.ProgressPercentage < 100 ?
                "Downloading... " + e.ProgressPercentage + "%" : "Installing...";
        };
        worker.RunWorkerCompleted += delegate(object s, RunWorkerCompletedEventArgs e) {
            if (e.Error == null) {
                Installed = true;
                Close();
                return;
            }
            updateButton.Enabled = laterButton.Enabled = skipButton.Enabled = true;
            progress.Visible = false;
            status.Text = "";
            string reason = e.Error is IOException || e.Error is UnauthorizedAccessException ?
                "A file could not be replaced. Close xemu and try again.\n\n" + e.Error.Message :
                e.Error.Message;
            MessageBox.Show(this, "The update was not installed.\n\n" + reason,
                            "Black Arrow Online", MessageBoxButtons.OK, MessageBoxIcon.Warning);
        };
        worker.RunWorkerAsync();
    }
}

// Installs the build chosen in xemu (Help > Versions & Updates), once xemu
// has closed
class InstallForm : Form
{
    public bool Installed;

    readonly int build, waitPid;
    readonly ProgressBar progress;
    readonly Label status;
    readonly Button retryButton, cancelButton;

    public InstallForm(int build, int waitPid)
    {
        this.build = build;
        this.waitPid = waitPid;
        Text = "Black Arrow Online - Install build " + UpdateInfo.CodeText(build);
        AutoScaleDimensions = new SizeF(96F, 96F);
        AutoScaleMode = AutoScaleMode.Dpi;
        ClientSize = new Size(520, 210);
        StartPosition = FormStartPosition.CenterScreen;
        FormBorderStyle = FormBorderStyle.FixedSingle;
        MaximizeBox = false;
        MinimizeBox = false;
        BackColor = Theme.Back;
        ForeColor = Theme.TextColor;
        Font = new Font("Segoe UI", 10F);
        Icon = Launcher.AppIcon();

        UpdateInfo installed = Updater.Installed();
        Theme.AddLabel(this, "Installing build " + UpdateInfo.CodeText(build), 24, 16, 472, 38, 18F, true,
                       Theme.TextColor);
        Theme.AddLabel(this, (installed.Build > 0 ? "Replacing build " + installed.Text + ". " : "") +
                             "Your console, Xbox Live sign-in, saves, settings and games " +
                             "are not touched.",
                       24, 60, 472, 44, 9F, false, Theme.Dim);
        status = Theme.AddLabel(this, "Waiting for xemu to close...", 24, 108, 472, 22, 9F,
                                false, Theme.TextColor);
        progress = new ProgressBar();
        progress.SetBounds(24, 136, 472, 8);
        progress.Style = ProgressBarStyle.Continuous;
        Controls.Add(progress);
        retryButton = Theme.AddButton(this, "Try again", 24, 156, 140, 38, true);
        cancelButton = Theme.AddButton(this, "Cancel", 176, 156, 110, 38, false);
        retryButton.Visible = cancelButton.Visible = false;
        retryButton.Click += delegate { Run(); };
        cancelButton.Click += delegate { Close(); };
    }

    protected override void OnShown(EventArgs e)
    {
        base.OnShown(e);
        Run();
    }

    void Run()
    {
        retryButton.Visible = cancelButton.Visible = false;
        progress.Value = 0;
        status.Text = "Waiting for xemu to close...";

        var worker = new BackgroundWorker();
        worker.WorkerReportsProgress = true;
        worker.DoWork += delegate {
            WaitForXemu();
            worker.ReportProgress(0);
            UpdateInfo update = Updater.ForBuild(build);
            Updater.Install(update, p => worker.ReportProgress(p));
        };
        worker.ProgressChanged += delegate(object s, ProgressChangedEventArgs e) {
            progress.Value = Math.Min(100, e.ProgressPercentage);
            status.Text = e.ProgressPercentage < 100 ?
                "Downloading... " + e.ProgressPercentage + "%" : "Installing...";
        };
        worker.RunWorkerCompleted += delegate(object s, RunWorkerCompletedEventArgs e) {
            if (e.Error == null) {
                Installed = true;
                Close();
                return;
            }
            progress.Value = 0;
            status.Text = e.Error is IOException || e.Error is UnauthorizedAccessException ?
                "A file could not be replaced: " + e.Error.Message : e.Error.Message;
            retryButton.Visible = cancelButton.Visible = true;
        };
        worker.RunWorkerAsync();
    }

    // xemu closes itself after starting this; files in use cannot be replaced
    void WaitForXemu()
    {
        if (waitPid > 0) {
            try {
                using (Process p = Process.GetProcessById(waitPid)) {
                    if (!p.WaitForExit(60000)) {
                        throw new IOException("xemu did not close. Close it and try again.");
                    }
                }
            } catch (ArgumentException) {
                // Already closed
            }
        }
        string xemu = Path.GetFullPath(Path.Combine(Launcher.systemDir, "xemu.exe"));
        for (int i = 0; i < 60; i++) {
            bool running = Process.GetProcessesByName("xemu").Any(p => {
                try {
                    return string.Equals(Path.GetFullPath(p.MainModule.FileName), xemu,
                                         StringComparison.OrdinalIgnoreCase);
                } catch (Exception) {
                    return false;
                }
            });
            if (!running) {
                return;
            }
            Thread.Sleep(500);
        }
        throw new IOException("xemu is still running. Close it and try again.");
    }
}

// A rounded panel
class Card : Panel
{
    public Card()
    {
        DoubleBuffered = true;
        ResizeRedraw = true;
        BackColor = Theme.Back;
    }

    protected override void OnPaint(PaintEventArgs e)
    {
        base.OnPaint(e);
        e.Graphics.SmoothingMode = SmoothingMode.AntiAlias;
        using (GraphicsPath path = Rounded(new RectangleF(0.5F, 0.5F, Width - 1.5F, Height - 1.5F), 10))
        using (var fill = new SolidBrush(Theme.PanelBack))
        using (var line = new Pen(Color.FromArgb(54, 58, 61))) {
            e.Graphics.FillPath(fill, path);
            e.Graphics.DrawPath(line, path);
        }
    }

    public static GraphicsPath Rounded(RectangleF r, float radius)
    {
        float d = radius * 2;
        var path = new GraphicsPath();
        path.AddArc(r.X, r.Y, d, d, 180, 90);
        path.AddArc(r.Right - d, r.Y, d, d, 270, 90);
        path.AddArc(r.Right - d, r.Bottom - d, d, d, 0, 90);
        path.AddArc(r.X, r.Bottom - d, d, d, 90, 90);
        path.CloseFigure();
        return path;
    }
}

// A green ring with a step number
class Badge : Control
{
    readonly string number;

    public Badge(string number)
    {
        this.number = number;
        SetStyle(ControlStyles.SupportsTransparentBackColor | ControlStyles.UserPaint |
                 ControlStyles.AllPaintingInWmPaint | ControlStyles.OptimizedDoubleBuffer, true);
        BackColor = Theme.Back;
    }

    protected override void OnPaint(PaintEventArgs e)
    {
        base.OnPaint(e);
        Graphics g = e.Graphics;
        g.SmoothingMode = SmoothingMode.AntiAlias;
        g.TextRenderingHint = System.Drawing.Text.TextRenderingHint.AntiAliasGridFit;
        var r = new RectangleF(1, 1, Width - 3, Height - 3);
        using (var fill = new SolidBrush(Color.FromArgb(30, 64, 34)))
        using (var ring = new Pen(Theme.Green, 2F))
        using (var font = new Font("Segoe UI", 11F, FontStyle.Bold))
        using (var text = new SolidBrush(Color.White)) {
            g.FillEllipse(fill, r);
            g.DrawEllipse(ring, r);
            var center = new StringFormat { Alignment = StringAlignment.Center,
                                             LineAlignment = StringAlignment.Center };
            g.DrawString(number, font, text, new RectangleF(0, 1, Width, Height), center);
        }
    }
}

// The logo: drawn in left to right behind a green edge, then a shine runs
// across it every few seconds. A click plays it again.
class LogoShow : Control
{
    const double RevealSeconds = 1.1;
    const double ShineEvery = 5.0;
    const double ShineSeconds = 1.1;

    readonly Image logo;
    readonly int bandHeight;  // height of the header glow, from the form's top
    readonly Stopwatch clock = Stopwatch.StartNew();
    bool finished, wasMoving = true;

    public LogoShow(Image logo, int bandHeight)
    {
        this.logo = logo;
        this.bandHeight = bandHeight;
        SetStyle(ControlStyles.UserPaint | ControlStyles.AllPaintingInWmPaint |
                 ControlStyles.OptimizedDoubleBuffer, true);
        Cursor = Cursors.Hand;
    }

    // Shown as it ends, for pictures of the window
    public void Finish()
    {
        finished = true;
        Invalidate();
    }

    // Called by the form's timer: repaints only while something moves
    public void Tick()
    {
        bool moving = Moving();
        if (moving || wasMoving) {
            Invalidate();
        }
        wasMoving = moving;
    }

    bool Moving()
    {
        if (finished) {
            return false;
        }
        double t = clock.Elapsed.TotalSeconds;
        return t < RevealSeconds || (t - RevealSeconds) % ShineEvery < ShineSeconds;
    }

    protected override void OnClick(EventArgs e)
    {
        base.OnClick(e);
        finished = false;
        clock.Restart();
    }

    Rectangle ImageRect()
    {
        float aspect = (float)logo.Width / logo.Height;
        int h = Height - 12, w = (int)(h * aspect);
        if (w > Width - 8) {
            w = Width - 8;
            h = (int)(w / aspect);
        }
        return new Rectangle((Width - w) / 2, (Height - h) / 2, w, h);
    }

    protected override void OnPaint(PaintEventArgs e)
    {
        Graphics g = e.Graphics;
        // The header glow, lined up with the form's
        var band = new Rectangle(-Left, -Top, Parent.ClientSize.Width, bandHeight);
        using (var brush = new LinearGradientBrush(band, SetupForm.HeaderTop, Theme.Back,
                                                   LinearGradientMode.Vertical)) {
            g.FillRectangle(brush, ClientRectangle);
        }
        g.SmoothingMode = SmoothingMode.AntiAlias;
        g.InterpolationMode = InterpolationMode.HighQualityBicubic;

        Rectangle img = ImageRect();
        double t = finished ? 99 : clock.Elapsed.TotalSeconds;

        // Reveal, eased, with a green edge
        double r = Math.Min(1.0, t / RevealSeconds);
        r = 1 - Math.Pow(1 - r, 3);
        int edge = img.X + (int)(img.Width * r);
        g.SetClip(new Rectangle(img.X, 0, edge - img.X, Height));
        g.DrawImage(logo, img);
        g.ResetClip();
        if (r < 1) {
            using (var glow = new Pen(Color.FromArgb(70, 90, 230, 110), 9F))
            using (var line = new Pen(Color.FromArgb(150, 255, 160), 2F)) {
                g.DrawLine(glow, edge, img.Y + 4, edge, img.Bottom - 4);
                g.DrawLine(line, edge, img.Y + 4, edge, img.Bottom - 4);
            }
            return;
        }

        // Shine: three slanted bands, brighter toward the middle
        double s = (t - RevealSeconds) % ShineEvery / ShineSeconds;
        if (finished || s >= 1) {
            return;
        }
        float cx = img.X - 60 + (float)((img.Width + 120) * s);
        float[] widths = { 70F, 40F, 16F };
        float[] lift = { 0.12F, 0.22F, 0.34F };   // how far toward a green-white glint
        for (int i = 0; i < widths.Length; i++) {
            float half = widths[i] / 2, slant = 22F;
            using (var path = new GraphicsPath())
            using (var attributes = new ImageAttributes()) {
                path.AddPolygon(new[] {
                    new PointF(cx - half + slant, img.Y), new PointF(cx + half + slant, img.Y),
                    new PointF(cx + half - slant, img.Bottom),
                    new PointF(cx - half - slant, img.Bottom),
                });
                // Darken a little, then lift toward green-white: the silver
                // letters are near white already, so a plain lift would not show
                var m = new ColorMatrix();
                m.Matrix00 = m.Matrix11 = m.Matrix22 = 1 - lift[i];
                m.Matrix40 = lift[i] * 0.55F;
                m.Matrix41 = lift[i] * 1.0F;
                m.Matrix42 = lift[i] * 0.6F;
                attributes.SetColorMatrix(m);
                g.SetClip(path);
                g.DrawImage(logo, img, 0, 0, logo.Width, logo.Height, GraphicsUnit.Pixel,
                            attributes);
                g.ResetClip();
            }
        }
    }
}

// A rounded progress bar: a green fill that eases to its value, with a light
// running along it
class GlowBar : Control
{
    readonly Stopwatch clock = Stopwatch.StartNew();
    int value;
    float shown;

    public GlowBar()
    {
        SetStyle(ControlStyles.UserPaint | ControlStyles.AllPaintingInWmPaint |
                 ControlStyles.OptimizedDoubleBuffer, true);
    }

    public int Value
    {
        get { return value; }
        set { this.value = Math.Max(0, Math.Min(100, value)); }
    }

    public void Reset()
    {
        value = 0;
        shown = 0;
        Invalidate();
    }

    public void Tick()
    {
        if (Visible) {
            shown += (value - shown) * 0.18F;
            Invalidate();
        }
    }

    protected override void OnPaint(PaintEventArgs e)
    {
        Graphics g = e.Graphics;
        g.Clear(Theme.PanelBack);
        g.SmoothingMode = SmoothingMode.AntiAlias;
        var track = new RectangleF(0.5F, 0.5F, Width - 1.5F, Height - 1.5F);
        float radius = track.Height / 2;
        using (GraphicsPath path = Card.Rounded(track, radius))
        using (var fill = new SolidBrush(Theme.Back))
        using (var line = new Pen(Color.FromArgb(54, 58, 61))) {
            g.FillPath(fill, path);
            g.DrawPath(line, path);
        }
        if (shown < 0.5F) {
            return;
        }
        float w = Math.Max(track.Height, track.Width * shown / 100F);
        var filled = new RectangleF(track.X, track.Y, w, track.Height);
        using (GraphicsPath path = Card.Rounded(filled, radius))
        using (var fill = new LinearGradientBrush(new RectangleF(0, 0, Width, Height),
                                                  Color.FromArgb(40, 130, 52),
                                                  Color.FromArgb(96, 214, 106),
                                                  LinearGradientMode.Horizontal)) {
            g.FillPath(fill, path);
            // A light running along the filled part
            float run = 90F;
            float x = (float)(clock.Elapsed.TotalSeconds % 1.4 / 1.4) * (w + run) - run;
            var light = new RectangleF(x, 0, run, Height);
            using (var sheen = new LinearGradientBrush(light, Color.Transparent,
                                                       Color.Transparent,
                                                       LinearGradientMode.Horizontal)) {
                var blend = new ColorBlend(3);
                blend.Colors = new[] { Color.FromArgb(0, 255, 255, 255),
                                       Color.FromArgb(110, 255, 255, 255),
                                       Color.FromArgb(0, 255, 255, 255) };
                blend.Positions = new[] { 0F, 0.5F, 1F };
                sheen.InterpolationColors = blend;
                g.SetClip(path);
                g.FillRectangle(sheen, light);
                g.ResetClip();
            }
        }
    }
}

class SetupForm : Form
{
    public static readonly Color HeaderTop = Color.FromArgb(22, 44, 26);
    static readonly Color Soft = Color.FromArgb(200, 204, 206);
    static readonly Color Back = Theme.Back;
    static readonly Color PanelBack = Theme.PanelBack;
    static readonly Color Green = Theme.Green;
    static readonly Color Grey = Theme.Grey;
    static readonly Color TextColor = Theme.TextColor;
    static readonly Color Dim = Theme.Dim;

    public bool Completed;

    readonly string preview;
    readonly Button cleanButton;
    readonly GlowBar bar;
    readonly LogoShow logoShow;
    readonly System.Windows.Forms.Timer anim;
    readonly Label status, gamesLabel, cleanText, cleanTitle;

    const string IdleStatus = "This window shows only once. Help > Black Arrow Online " +
                              "Guide in xemu explains the rest.";

    const int W = 760;           // window width
    const int Side = 36;         // left and right margin
    const int Inner = W - 2 * Side;
    const int HeaderHeight = 150;

    public SetupForm(string previewPath)
    {
        preview = previewPath;
        Text = "Black Arrow Online - First-time setup";
        AutoScaleDimensions = new SizeF(96F, 96F);
        AutoScaleMode = AutoScaleMode.Dpi;
        ClientSize = new Size(W, 712);
        StartPosition = FormStartPosition.CenterScreen;
        FormBorderStyle = FormBorderStyle.FixedSingle;
        MaximizeBox = false;
        BackColor = Back;
        ForeColor = TextColor;
        Font = new Font("Segoe UI", 10F);
        Icon = Launcher.AppIcon();
        DoubleBuffered = true;

        // --- header: the logo on a soft green glow ---
        var logo = Launcher.Logo();
        if (logo != null) {
            logoShow = new LogoShow(logo, HeaderHeight);
            logoShow.SetBounds((W - 400) / 2, 14, 400, 102);  // the picture is 4:1
            Controls.Add(logoShow);
        } else {
            Label name = AddLabel(this, "Black Arrow Online", 0, 40, W, 50, 22F, true, TextColor);
            name.TextAlign = ContentAlignment.MiddleCenter;
        }
        Label sub = AddLabel(this, "Let's set up your virtual Xbox", 0, 116, W, 24, 10.5F,
                             false, Dim);
        sub.TextAlign = ContentAlignment.MiddleCenter;

        // --- new console: the one thing to do here ---
        var clean = AddCard(Side, 168, Inner, 124);
        cleanTitle = AddLabel(clean, "New console", 24, 20, 420, 32, 15F, true, TextColor);
        cleanText = AddLabel(clean, "", 25, 56, 420, 56, 9.5F, false, Dim);
        cleanButton = AddButton(clean, "", Inner - 24 - 196, 36, 196, 52, true);
        cleanButton.Font = new Font("Segoe UI", 11.5F, FontStyle.Bold);
        cleanButton.Click += delegate {
            if (Launcher.ContentMissing) {
                AddContent();
            } else {
                Run("Setting up your console", "Copying the console's hard disk...",
                    p => Launcher.StartClean(p), null);
            }
        };
        bar = new GlowBar();
        bar.SetBounds(25, 90, Inner - 50, 12);
        bar.Visible = false;
        clean.Controls.Add(bar);
        ShowClean();

        // --- what happens next: four steps in a row ---
        AddLabel(this, "WHAT HAPPENS NEXT", Side + 2, 314, 300, 20, 8.5F, true, Green);
        int col = (Inner + 16) / 4;
        AddStep(Side + 0 * col, 344, col - 16, "1", "XBL Dashboard opens",
                "It sets itself up the first time, about a minute. Then sign in " +
                "with your xb.live account.");
        AddStep(Side + 1 * col, 344, col - 16, "2", "Go online",
                "In the dashboard: Settings > Network, set the top option to XB Online.");
        AddStep(Side + 2 * col, 344, col - 16, "3", "Update the game",
                "In xemu: Play > Black Arrow Updates (stock) adds the update and maps.");
        AddStep(Side + 3 * col, 344, col - 16, "4", "Play",
                "Play > Play Rainbow Six 3 Black Arrow. From then on xemu opens " +
                "straight to the dashboard.");

        // --- good to know ---
        var know = AddCard(Side, 500, Inner, 100);
        int third = (Inner - 24) / 3;
        AddNote(know, 22 + 0 * third, third - 20, "Already on xb.live?",
                "Nothing to copy. Your account lives on xb.live; just sign in again.");
        AddNote(know, 22 + 1 * third, third - 20, "Don't close xemu",
                "Let the dashboard finish setting up. The old dashboard is kept as a backup.");
        AddNote(know, 22 + 2 * third, third - 20, "Saves",
                "Saves from another xemu aren't copied. This console starts new.");

        // --- your game ---
        var games = AddCard(Side, 614, Inner, 58);
        gamesLabel = AddLabel(games, "", 22, 9, Inner - 230, 40, 9.5F, true, TextColor);
        gamesLabel.TextAlign = ContentAlignment.MiddleLeft;
        Button openGames = AddButton(games, "Open games folder", Inner - 22 - 170, 12, 170, 34,
                                     false);
        openGames.Click += delegate {
            Directory.CreateDirectory(Launcher.GamesDir);
            Process.Start("explorer.exe", "\"" + Launcher.GamesDir + "\"");
        };
        Activated += delegate { ShowGames(); };
        ShowGames();

        // --- footer ---
        status = AddLabel(this, IdleStatus, Side + 2, 684, Inner - 140, 20, 8.5F, false, Dim);
        UpdateInfo installed = Updater.Installed();
        if (installed.Build > 0) {
            Label build = AddLabel(this, "Build " + installed.Text, W - Side - 140, 684, 140, 20,
                                   8.5F, true, Dim);
            build.TextAlign = ContentAlignment.TopRight;
        }

        anim = new System.Windows.Forms.Timer();
        anim.Interval = 16;
        anim.Tick += delegate {
            if (logoShow != null) {
                logoShow.Tick();
            }
            bar.Tick();
        };
        FormClosed += delegate { anim.Stop(); };
    }

    // The header glow behind the logo
    protected override void OnPaint(PaintEventArgs e)
    {
        base.OnPaint(e);
        var band = new Rectangle(0, 0, ClientSize.Width, HeaderHeight);
        using (var brush = new LinearGradientBrush(band, HeaderTop, Back,
                                                   LinearGradientMode.Vertical)) {
            e.Graphics.FillRectangle(brush, band);
        }
    }

    protected override void OnShown(EventArgs e)
    {
        base.OnShown(e);
        if (preview != null) {
            if (logoShow != null) {
                logoShow.Finish();
            }
            Theme.SavePreview(this, preview);
            Close();
            return;
        }
        anim.Start();
    }

    // Without the content pack the clean start button asks for it instead
    void ShowClean()
    {
        if (Launcher.ContentMissing) {
            cleanText.ForeColor = Color.FromArgb(230, 180, 70);
            cleanText.Text = "Needs the content pack first: " + Launcher.ContentPackName + ".\n" +
                             "Download it from where you got Black Arrow Online, then add it " +
                             "here. It is only needed once.";
            cleanButton.Text = "Add content pack";
        } else {
            cleanText.ForeColor = Dim;
            cleanText.Text = "A fresh virtual Xbox with the dashboards already installed. " +
                             "Your xb.live account signs in later.";
            cleanButton.Text = "Set up my console";
        }
    }

    void AddContent()
    {
        string zip = Launcher.FindContentPack();
        if (zip == null) {
            using (var dialog = new OpenFileDialog()) {
                dialog.Title = "Choose the content pack (" + Launcher.ContentPackName + ")";
                dialog.Filter = "Content pack (*.zip)|*.zip|All files (*.*)|*.*";
                if (Directory.Exists(Launcher.DownloadsDir)) {
                    dialog.InitialDirectory = Launcher.DownloadsDir;
                }
                if (dialog.ShowDialog(this) != DialogResult.OK) {
                    return;
                }
                zip = dialog.FileName;
            }
        }
        Run("Adding the content pack", "Unpacking " + Launcher.ContentPackName + "...",
            p => Launcher.InstallContent(zip, p), ShowClean);
    }

    // Runs work in the background, with its progress in the top card. With no
    // "then", success finishes the setup.
    void Run(string title, string what, Action<Action<int>> work, Action then)
    {
        cleanButton.Visible = false;
        cleanTitle.Text = title;
        cleanText.ForeColor = Soft;
        cleanText.SetBounds(25, 56, Inner - 50, 26);
        cleanText.Text = what;
        bar.Reset();
        bar.Visible = true;

        var worker = new BackgroundWorker();
        worker.WorkerReportsProgress = true;
        worker.DoWork += delegate { work(p => worker.ReportProgress(p)); };
        worker.ProgressChanged += delegate(object s, ProgressChangedEventArgs e) {
            bar.Value = e.ProgressPercentage;
            cleanText.Text = what + "  " + Math.Min(100, e.ProgressPercentage) + "%";
        };
        worker.RunWorkerCompleted += delegate(object s, RunWorkerCompletedEventArgs e) {
            if (e.Error == null && then == null) {
                // Let the bar fill, then hand over to xemu
                Completed = true;
                bar.Value = 100;
                cleanTitle.Text = "Your console is ready";
                cleanText.ForeColor = Green;
                cleanText.Text = "Starting xemu...";
                var close = new System.Windows.Forms.Timer();
                close.Interval = 900;
                close.Tick += delegate { close.Stop(); Close(); };
                close.Start();
                return;
            }
            if (then == null) {
                Launcher.RemovePartialHdd();
            }
            bar.Visible = false;
            cleanButton.Visible = true;
            cleanTitle.Text = "New console";
            cleanText.SetBounds(25, 56, 420, 56);
            ShowClean();
            if (e.Error != null) {
                MessageBox.Show(this, e.Error.Message, "Black Arrow Online",
                                MessageBoxButtons.OK, MessageBoxIcon.Warning);
            } else {
                then();
            }
        };
        worker.RunWorkerAsync();
    }

    void ShowGames()
    {
        string game = Launcher.FindGame();
        if (game != null) {
            gamesLabel.ForeColor = Green;
            gamesLabel.Text = "Game found: " + game;
        } else {
            gamesLabel.ForeColor = Color.FromArgb(230, 180, 70);
            gamesLabel.Text = "No game yet: copy your Black Arrow disc image (.iso) " +
                              "into the games folder.";
        }
    }

    // --- controls ---

    // A numbered circle, then a short title and line
    void AddStep(int x, int y, int w, string number, string title, string text)
    {
        var badge = new Badge(number);
        badge.SetBounds(x, y, 32, 32);
        Controls.Add(badge);
        AddLabel(this, title, x, y + 42, w, 22, 10.5F, true, TextColor);
        AddLabel(this, text, x, y + 66, w, 84, 9.5F, false, Soft);
    }

    static void AddNote(Control parent, int x, int w, string title, string text)
    {
        AddLabel(parent, title, x, 16, w, 22, 9.5F, true, TextColor);
        AddLabel(parent, text, x, 40, w, 62, 9F, false, Soft);
    }

    Card AddCard(int x, int y, int w, int h)
    {
        var card = new Card();
        card.SetBounds(x, y, w, h);
        Controls.Add(card);
        return card;
    }

    static Label AddLabel(Control parent, string text, int x, int y, int w, int h,
                          float size, bool bold, Color color)
    {
        return Theme.AddLabel(parent, text, x, y, w, h, size, bold, color);
    }

    static Button AddButton(Control parent, string text, int x, int y, int w, int h,
                            bool primary)
    {
        return Theme.AddButton(parent, text, x, y, w, h, primary);
    }

}
