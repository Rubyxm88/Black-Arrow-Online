// Black Arrow Online launcher.
//
// Starts xemu from the "system" folder next to this program. When no hard
// disk has been set up yet it first asks whether to start clean or to copy
// in the files of an existing xemu setup. Nothing outside the package folder
// is ever changed.
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
            } else if (mode == "insignia") {
                Updater.FetchInsigniaDisc();
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
            l => !Regex.IsMatch(l, "^(xb_online_setup_offered|dvd_path) = "));
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

    public static Icon AppIcon()
    {
        try {
            return Icon.ExtractAssociatedIcon(Application.ExecutablePath);
        } catch (Exception) {
            return null;
        }
    }
}

class UpdateInfo
{
    public int Build;
    public string Xemu = "", Date = "", Sha256 = "", Notes = "";
}

// Looks for, downloads and installs newer builds from the releases page
static class Updater
{
    public const string ReleasesPage =
        "https://github.com/Rubyxm88/Black-Arrow-Online/releases";
    const string PackageName = "BlackArrowOnline-update.zip";
    const int CheckTimeoutMs = 5000;

    // Insignia's Setup Assistant, fetched from Insignia's own releases rather
    // than ours, for folders set up from a package that did not include it
    const string InsigniaDisc = "Insignia-Setup.iso";
    const string InsigniaUrl = "https://github.com/insignia-live/setup-assistant-release" +
                               "/releases/download/2025-01-19/Insignia-Setup.iso";
    const string InsigniaSha256 =
        "66a84282bb2a7fe8fe0d6a70e8333485b13e333cb827245ca7de8af8b429e883";

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

    static WebRequest Request(string name, int timeoutMs)
    {
        // TLS 1.2, which older .NET versions do not offer by default
        ServicePointManager.SecurityProtocol |= (SecurityProtocolType)3072;
        WebRequest request = WebRequest.Create(BaseUrl + name);
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
                using (WebResponse response = Request("version.txt", CheckTimeoutMs).GetResponse())
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
            int.TryParse(File.ReadAllText(InSystem("update-skip.txt")).Trim(), out skipped);
        } catch (IOException) {
        }
        return latest.Build > Installed().Build && latest.Build != skipped ? latest : null;
    }

    public static void Skip(UpdateInfo update)
    {
        try {
            File.WriteAllText(InSystem("update-skip.txt"), update.Build.ToString());
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

        using (WebResponse response = Request(PackageName, 30000).GetResponse())
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
        FetchInsigniaDisc();
    }

    static string Sha256Of(string path)
    {
        using (var sha = SHA256.Create())
        using (var stream = File.OpenRead(path)) {
            return BitConverter.ToString(sha.ComputeHash(stream)).Replace("-", "")
                               .ToLowerInvariant();
        }
    }

    // Adds the Insignia setup disc when it is missing. It only enables the
    // New Account entries of the XB Online menu, so any failure is ignored.
    public static void FetchInsigniaDisc()
    {
        string target = InSystem(InsigniaDisc), part = target + ".part";
        if (File.Exists(target)) {
            return;
        }
        try {
            ServicePointManager.SecurityProtocol |= (SecurityProtocolType)3072;
            var request = (HttpWebRequest)WebRequest.Create(InsigniaUrl);
            request.Timeout = 30000;
            request.ReadWriteTimeout = 30000;
            request.UserAgent = "BlackArrowOnline-Launcher";
            using (WebResponse response = request.GetResponse())
            using (Stream src = response.GetResponseStream())
            using (var dst = new FileStream(part, FileMode.Create, FileAccess.Write)) {
                src.CopyTo(dst);
            }
            if (Sha256Of(part) == InsigniaSha256) {
                File.Move(part, target);
            }
        } catch (Exception) {
            // Offline or moved: the menu entries stay hidden
        } finally {
            try {
                File.Delete(part);
            } catch (IOException) {
            }
        }
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
        Theme.AddLabel(panel, "Build " + update.Build +
                              (update.Date.Length > 0 ? "  -  " + update.Date : ""),
                       18, 12, 476, 26, 12F, true, Theme.Green);
        Theme.AddLabel(panel, (update.Xemu.Length > 0 ? "Based on xemu " + update.Xemu + ".  " : "") +
                              "You have " +
                              (installed.Build > 0 ? "build " + installed.Build : "an earlier build") + ".",
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

class SetupForm : Form
{
    static readonly Color Back = Theme.Back;
    static readonly Color PanelBack = Theme.PanelBack;
    static readonly Color FieldBack = Color.FromArgb(50, 54, 57);
    static readonly Color Green = Theme.Green;
    static readonly Color Grey = Theme.Grey;
    static readonly Color TextColor = Theme.TextColor;
    static readonly Color Dim = Theme.Dim;

    public bool Completed;

    readonly string preview;
    readonly TextBox hddBox, eepromBox, biosBox, mcpxBox;
    readonly Button cleanButton, importButton;
    readonly ProgressBar progress;
    readonly Label status, gamesLabel, cleanText;

    const string IdleStatus = "Close xemu before copying files that it is using.";

    public SetupForm(string previewPath)
    {
        preview = previewPath;
        Text = "Black Arrow Online - First-time setup";
        AutoScaleDimensions = new SizeF(96F, 96F);
        AutoScaleMode = AutoScaleMode.Dpi;
        ClientSize = new Size(1012, 708);
        StartPosition = FormStartPosition.CenterScreen;
        FormBorderStyle = FormBorderStyle.FixedSingle;
        MaximizeBox = false;
        BackColor = Back;
        ForeColor = TextColor;
        Font = new Font("Segoe UI", 10F);
        Icon = Launcher.AppIcon();

        AddLabel(this, "Black Arrow Online", 24, 16, 630, 38, 18F, true, TextColor);
        AddLabel(this, "First-time setup: how do you want to set up your virtual Xbox?",
                 26, 56, 630, 24, 10F, false, Dim);

        // --- clean start ---
        var clean = AddPanel(24, 92, 632, 112);
        AddLabel(clean, "Clean start", 18, 12, 420, 28, 13F, true, TextColor);
        cleanText = AddLabel(clean, "", 18, 44, 440, 56, 9F, false, Dim);
        cleanButton = AddButton(clean, "", 468, 36, 146, 42, true);
        cleanButton.Click += delegate {
            if (Launcher.ContentMissing) {
                AddContent();
            } else {
                Run("Copying files...", p => Launcher.StartClean(p), null);
            }
        };
        ShowClean();

        // --- existing files ---
        var import = AddPanel(24, 216, 632, 428);
        AddLabel(import, "Use my existing xemu files", 18, 12, 590, 28, 13F, true, TextColor);
        AddLabel(import, "Copies your files into this folder, keeping your Xbox Live account and " +
                         "saves. Your originals are not changed. In xemu, Machine > Settings > " +
                         "System > Files shows where yours are.",
                 18, 44, 596, 42, 9F, false, Dim);

        hddBox = AddFileRow(import, 96, "Hard disk", "Required",
                            "xbox_hdd.qcow2", "the Xbox hard disk image, a .qcow2 file (often 1 GB or more)",
                            "Xbox hard disk image (*.qcow2)|*.qcow2|All files (*.*)|*.*",
                            preview != null ? "" : Launcher.ExistingPath("hdd_path"));
        eepromBox = AddFileRow(import, 168, "EEPROM", "Required",
                               "eeprom.bin", "the console identity that belongs to that hard disk, a 256-byte .bin file",
                               "Xbox EEPROM (*.bin)|*.bin|All files (*.*)|*.*",
                               preview != null ? "" : Launcher.ExistingPath("eeprom_path"));
        biosBox = AddFileRow(import, 240, "BIOS", "Optional",
                             "for example Complex_4627.bin", "a 256 KB or 1 MB .bin file. Leave empty to use the included BIOS",
                             "Xbox BIOS (*.bin)|*.bin|All files (*.*)|*.*", "");
        mcpxBox = AddFileRow(import, 312, "MCPX boot ROM", "Optional",
                             "mcpx_1.0.bin", "a 512-byte .bin file. Leave empty to use the included one",
                             "MCPX boot ROM (*.bin)|*.bin|All files (*.*)|*.*", "");

        importButton = AddButton(import, "Copy and start", 454, 372, 160, 40, true);
        importButton.Click += delegate {
            string hdd = hddBox.Text.Trim(), eeprom = eepromBox.Text.Trim();
            string bios = biosBox.Text.Trim(), mcpx = mcpxBox.Text.Trim();
            Run("Copying files...", p => Launcher.ImportExisting(hdd, eeprom, bios, mcpx, p), null);
        };

        // --- what happens next ---
        var next = AddPanel(668, 92, 320, 552);
        AddLabel(next, "What happens next", 18, 12, 290, 28, 13F, true, TextColor);
        AddStep(next, 50, "1", "The XBL Dashboard opens",
                "Sign in with your xb.live account. For a link code use the menu " +
                "XB Online > Get a Link Code.");
        AddStep(next, 138, "2", "Switch the network to XB Online",
                "In the dashboard: Settings > Network, set the top option to XB Online.");
        AddStep(next, 212, "3", "Install the game update and maps",
                "Menu XB Online > Install Game Update + Maps. Skip this if your own " +
                "hard disk already has them.");
        AddStep(next, 300, "4", "Play",
                "Menu XB Online > Play Rainbow Six 3 Black Arrow.");
        AddLabel(next, "This window appears only once. From then on, Black Arrow Online " +
                       "goes straight to the game, and everything above stays available " +
                       "in the XB Online menu.",
                 18, 366, 286, 72, 9F, false, Dim);

        gamesLabel = AddLabel(next, "", 18, 448, 286, 40, 9F, true, TextColor);
        Button openGames = AddButton(next, "Open the games folder", 18, 496, 190, 34, false);
        openGames.Click += delegate {
            Directory.CreateDirectory(Launcher.GamesDir);
            Process.Start("explorer.exe", "\"" + Launcher.GamesDir + "\"");
        };
        Activated += delegate { ShowGames(); };
        ShowGames();

        status = AddLabel(this, IdleStatus, 26, 656, 630, 22, 9F, false, Dim);
        UpdateInfo installed = Updater.Installed();
        if (installed.Build > 0) {
            Label build = AddLabel(this, "Build " + installed.Build, 668, 656, 320, 22, 9F,
                                   false, Dim);
            build.TextAlign = ContentAlignment.TopRight;
        }
        progress = new ProgressBar();
        progress.SetBounds(26, 682, 628, 10);
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
            cleanText.Text = "A brand new console with the Xbox dashboards already installed.\n" +
                             "Choose this if you have never used xemu, or want a fresh Xbox " +
                             "Live setup.";
            cleanButton.Text = "Start clean";
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
        Run("Adding the content pack...", p => Launcher.InstallContent(zip, p), ShowClean);
    }

    // Runs work in the background. With no "then", success finishes the setup
    void Run(string what, Action<Action<int>> work, Action then)
    {
        cleanButton.Enabled = false;
        importButton.Enabled = false;
        progress.Value = 0;
        progress.Visible = true;
        status.Text = what;

        var worker = new BackgroundWorker();
        worker.WorkerReportsProgress = true;
        worker.DoWork += delegate { work(p => worker.ReportProgress(p)); };
        worker.ProgressChanged += delegate(object s, ProgressChangedEventArgs e) {
            progress.Value = Math.Min(100, e.ProgressPercentage);
            status.Text = what + " " + e.ProgressPercentage + "%";
        };
        worker.RunWorkerCompleted += delegate(object s, RunWorkerCompletedEventArgs e) {
            if (e.Error == null && then == null) {
                Completed = true;
                Close();
                return;
            }
            if (then == null) {
                Launcher.RemovePartialHdd();
            }
            cleanButton.Enabled = true;
            importButton.Enabled = true;
            progress.Visible = false;
            status.Text = IdleStatus;
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

    static void AddStep(Control parent, int y, string number, string title, string text)
    {
        AddLabel(parent, number, 18, y, 22, 24, 12F, true, Green);
        AddLabel(parent, title, 42, y + 2, 266, 22, 9.5F, true, TextColor);
        AddLabel(parent, text, 42, y + 24, 266, 56, 9F, false, Dim);
    }

    Panel AddPanel(int x, int y, int w, int h)
    {
        var panel = new Panel();
        panel.SetBounds(x, y, w, h);
        panel.BackColor = PanelBack;
        Controls.Add(panel);
        return panel;
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

    // One file: a title with a Required/Optional tag, the path box with a
    // Browse button, and a line saying which file is wanted
    TextBox AddFileRow(Control parent, int y, string title, string tag, string fileName,
                       string description, string filter, string initial)
    {
        Label name = AddLabel(parent, title, 18, y, 10, 20, 9.5F, true, TextColor);
        name.AutoSize = true;
        Label tagLabel = AddLabel(parent, tag.ToUpperInvariant(), 0, y + 3, 10, 16, 7.5F, true,
                                  tag == "Required" ? Green : Dim);
        tagLabel.AutoSize = true;
        parent.Layout += delegate { tagLabel.Left = name.Right + 6; };

        var box = new TextBox();
        box.SetBounds(18, y + 22, 498, 24);
        box.BackColor = FieldBack;
        box.ForeColor = TextColor;
        box.BorderStyle = BorderStyle.FixedSingle;
        box.Font = new Font("Segoe UI", 9F);
        box.Text = initial;
        parent.Controls.Add(box);

        Button browse = AddButton(parent, "Browse...", 524, y + 21, 90, 26, false);
        browse.Click += delegate {
            using (var dialog = new OpenFileDialog()) {
                dialog.Title = "Choose your " + title + " file (" + fileName + ")";
                dialog.Filter = filter;
                string current = box.Text.Trim();
                if (current.Length > 0 && File.Exists(current)) {
                    dialog.InitialDirectory = Path.GetDirectoryName(current);
                }
                if (dialog.ShowDialog(this) == DialogResult.OK) {
                    box.Text = dialog.FileName;
                }
            }
        };

        AddLabel(parent, fileName + "  -  " + description, 18, y + 48, 596, 16, 8F, false, Dim);
        return box;
    }
}
