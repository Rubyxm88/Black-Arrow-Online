// Black Arrow Online launcher.
//
// Starts xemu from the "system" folder next to this program. When no hard
// disk has been set up yet it first asks whether to start clean or to copy
// in the files of an existing xemu setup. Nothing outside the package folder
// is ever changed.
//
// Build: csc /target:winexe /win32icon:xemu-icon.ico Launcher.cs
using System;
using System.ComponentModel;
using System.Diagnostics;
using System.Drawing;
using System.Drawing.Imaging;
using System.IO;
using System.Linq;
using System.Runtime.InteropServices;
using System.Text.RegularExpressions;
using System.Windows.Forms;

static class Launcher
{
    const string BundledBios = "Complex_4627v1.03.bin";
    const string BundledMcpx = "mcpx.bin";

    static string systemDir, hddTarget, eepromTarget, toml;

    [DllImport("user32.dll")]
    static extern bool SetProcessDPIAware();

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

        // Unattended modes, used for testing
        string mode = Arg(args, "--mode"), preview = Arg(args, "--preview");
        bool noLaunch = args.Contains("--nolaunch");
        try {
            if (mode == "clean") {
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

    public static void StartClean(Action<int> progress)
    {
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

class SetupForm : Form
{
    static readonly Color Back = Color.FromArgb(24, 26, 27);
    static readonly Color PanelBack = Color.FromArgb(36, 39, 41);
    static readonly Color FieldBack = Color.FromArgb(50, 54, 57);
    static readonly Color Green = Color.FromArgb(60, 170, 70);
    static readonly Color Grey = Color.FromArgb(62, 66, 69);
    static readonly Color TextColor = Color.FromArgb(235, 235, 235);
    static readonly Color Dim = Color.FromArgb(165, 170, 172);

    public bool Completed;

    readonly string preview;
    readonly TextBox hddBox, eepromBox, biosBox, mcpxBox;
    readonly Button cleanButton, importButton;
    readonly ProgressBar progress;
    readonly Label status, gamesLabel;

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
        AddLabel(clean, "A brand new console with the Xbox dashboards already installed.\n" +
                        "Choose this if you have never used xemu, or want a fresh Xbox Live setup.",
                 18, 44, 440, 56, 9F, false, Dim);
        cleanButton = AddButton(clean, "Start clean", 468, 36, 146, 42, true);
        cleanButton.Click += delegate { Run(p => Launcher.StartClean(p)); };

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
                            Launcher.ExistingPath("hdd_path"));
        eepromBox = AddFileRow(import, 168, "EEPROM", "Required",
                               "eeprom.bin", "the console identity that belongs to that hard disk, a 256-byte .bin file",
                               "Xbox EEPROM (*.bin)|*.bin|All files (*.*)|*.*",
                               Launcher.ExistingPath("eeprom_path"));
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
            Run(p => Launcher.ImportExisting(hdd, eeprom, bios, mcpx, p));
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

        status = AddLabel(this, "Close xemu before copying files that it is using.",
                          26, 656, 630, 22, 9F, false, Dim);
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
            Refresh();
            using (var bitmap = new Bitmap(Width, Height)) {
                DrawToBitmap(bitmap, new Rectangle(0, 0, Width, Height));
                bitmap.Save(preview, ImageFormat.Png);
            }
            Close();
        }
    }

    void Run(Action<Action<int>> work)
    {
        cleanButton.Enabled = false;
        importButton.Enabled = false;
        progress.Value = 0;
        progress.Visible = true;
        status.Text = "Copying files...";

        var worker = new BackgroundWorker();
        worker.WorkerReportsProgress = true;
        worker.DoWork += delegate { work(p => worker.ReportProgress(p)); };
        worker.ProgressChanged += delegate(object s, ProgressChangedEventArgs e) {
            progress.Value = Math.Min(100, e.ProgressPercentage);
            status.Text = "Copying files... " + e.ProgressPercentage + "%";
        };
        worker.RunWorkerCompleted += delegate(object s, RunWorkerCompletedEventArgs e) {
            if (e.Error == null) {
                Completed = true;
                Close();
                return;
            }
            Launcher.RemovePartialHdd();
            cleanButton.Enabled = true;
            importButton.Enabled = true;
            progress.Visible = false;
            status.Text = "Close xemu before copying files that it is using.";
            MessageBox.Show(this, e.Error.Message, "Black Arrow Online",
                            MessageBoxButtons.OK, MessageBoxIcon.Warning);
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

    static Button AddButton(Control parent, string text, int x, int y, int w, int h,
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
