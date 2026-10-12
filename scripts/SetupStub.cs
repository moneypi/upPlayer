// Self-extracting setup. Layout: [stub.exe][zip payload][Int64 zipOffset][Int64 magic]
// UI: choose install folder + optional launch. No PowerShell.
using System;
using System.ComponentModel;
using System.Diagnostics;
using System.Drawing;
using System.IO;
using System.IO.Compression;
using System.Reflection;
using System.Security.Principal;
using System.Threading;
using System.Windows.Forms;
using Microsoft.Win32;

internal static class Program
{
    private const long Magic = 0x5550504C53544658; // "UPPLSTFX"
    private const string AppName = "upPlayer";
    private const string ExeName = "upPlayer.exe";

    private static readonly string[] Extensions = new string[]
    {
        ".mp4", ".m4v", ".mkv", ".webm", ".avi", ".mov", ".wmv", ".flv", ".ts", ".m2ts", ".mts",
        ".mpg", ".mpeg", ".mpe", ".vob", ".rmvb", ".rm", ".3gp", ".3g2", ".asf", ".f4v", ".ogv",
        ".divx", ".wtv", ".dvr-ms", ".m3u8", ".m3u"
    };

    [STAThread]
    private static int Main(string[] args)
    {
        Application.EnableVisualStyles();
        Application.SetCompatibleTextRenderingDefault(false);

        string version = "1.0.0";
        for (int i = 0; i < args.Length - 1; i++)
        {
            if (args[i] == "--version")
                version = args[i + 1];
        }

        try
        {
            if (HasArg(args, "--uninstall"))
                return RunUninstall(args);

            if (!IsAdmin())
                return RelaunchElevated(args);

            using (var form = new SetupWizard(version))
            {
                Application.Run(form);
                return form.ExitCode;
            }
        }
        catch (Exception ex)
        {
            MessageBox.Show(ex.Message, "upPlayer Setup", MessageBoxButtons.OK, MessageBoxIcon.Error);
            return 1;
        }
    }

    private static bool HasArg(string[] args, string name)
    {
        for (int i = 0; i < args.Length; i++)
        {
            if (string.Equals(args[i], name, StringComparison.OrdinalIgnoreCase))
                return true;
        }
        return false;
    }

    private static bool IsAdmin()
    {
        using (var id = WindowsIdentity.GetCurrent())
        {
            var principal = new WindowsPrincipal(id);
            return principal.IsInRole(WindowsBuiltInRole.Administrator);
        }
    }

    private static int RelaunchElevated(string[] args)
    {
        string self = GetSelfPath();
        var psi = new ProcessStartInfo
        {
            FileName = self,
            UseShellExecute = true,
            Verb = "runas",
            Arguments = QuoteArgs(args)
        };
        try
        {
            using (var p = Process.Start(psi))
            {
                if (p == null)
                    return 1;
                p.WaitForExit();
                return p.ExitCode;
            }
        }
        catch
        {
            return 1; // user cancelled UAC
        }
    }

    private static string QuoteArgs(string[] args)
    {
        if (args == null || args.Length == 0)
            return string.Empty;
        var parts = new string[args.Length];
        for (int i = 0; i < args.Length; i++)
        {
            string a = args[i] ?? string.Empty;
            if (a.IndexOfAny(new char[] { ' ', '"' }) >= 0)
                parts[i] = "\"" + a.Replace("\"", "\\\"") + "\"";
            else
                parts[i] = a;
        }
        return string.Join(" ", parts);
    }

    private static string GetSelfPath()
    {
        string self = Assembly.GetExecutingAssembly().Location;
        if (string.IsNullOrEmpty(self))
            self = Process.GetCurrentProcess().MainModule.FileName;
        return self;
    }

    // Launch from an elevated installer at medium integrity (explorer proxy).
    private static void LaunchUnelevated(string exePath)
    {
        try
        {
            Process.Start(new ProcessStartInfo
            {
                FileName = "explorer.exe",
                Arguments = "\"" + exePath + "\"",
                UseShellExecute = true
            });
        }
        catch
        {
            Process.Start(new ProcessStartInfo
            {
                FileName = exePath,
                WorkingDirectory = Path.GetDirectoryName(exePath),
                UseShellExecute = true
            });
        }
    }

    private delegate void ProgressFn(int percent, string status, string detail);

    private static string InstallCore(string installDir, string version, ProgressFn progress)
    {
        if (progress == null)
            progress = delegate { };

        string self = GetSelfPath();
        string tempRoot = Path.Combine(Path.GetTempPath(), "upPlayer-setup-" + Guid.NewGuid().ToString("N"));
        Directory.CreateDirectory(tempRoot);
        try
        {
            progress(5, "Preparing...", "Reading setup package");
            string zipPath = Path.Combine(tempRoot, "payload.zip");
            ExtractPayload(self, zipPath);

            progress(15, "Extracting files...", "Unpacking payload");
            string extractDir = Path.Combine(tempRoot, "payload");
            Directory.CreateDirectory(extractDir);
            ZipFile.ExtractToDirectory(zipPath, extractDir);

            progress(35, "Installing files...", installDir);
            Directory.CreateDirectory(installDir);
            CopyDirectory(extractDir, installDir, progress);

            string exe = Path.Combine(installDir, ExeName);
            if (!File.Exists(exe))
                throw new FileNotFoundException("Missing " + ExeName + " in setup payload.");

            string uninstaller = Path.Combine(installDir, "Uninstall.exe");
            if (!File.Exists(uninstaller))
                WriteUninstallCmd(installDir);

            progress(80, "Registering Open with...", "File associations");
            RegisterAssociations(exe);

            progress(90, "Creating shortcuts...", "Start Menu");
            CreateStartMenuShortcut(exe, installDir);
            WriteUninstallRegistry(installDir, exe, version);

            progress(100, "Finished", "Installation complete");
            Thread.Sleep(400);
            return exe;
        }
        finally
        {
            try { Directory.Delete(tempRoot, true); } catch { }
        }
    }

    private static int RunUninstall(string[] args)
    {
        if (!IsAdmin())
            return RelaunchElevated(args);

        string installDir = AppDomain.CurrentDomain.BaseDirectory.TrimEnd('\\', '/');
        for (int i = 0; i < args.Length - 1; i++)
        {
            if (args[i] == "--dir")
                installDir = args[i + 1];
        }

        string exe = Path.Combine(installDir, ExeName);
        if (MessageBox.Show(
                "Uninstall upPlayer from:\n" + installDir + " ?",
                "upPlayer Uninstall",
                MessageBoxButtons.YesNo,
                MessageBoxIcon.Question) != DialogResult.Yes)
            return 1;

        try { UnregisterAssociations(exe); } catch { }
        try { RemoveStartMenuShortcut(); } catch { }
        try
        {
            using (RegistryKey key = Registry.LocalMachine.OpenSubKey(
                       @"Software\Microsoft\Windows\CurrentVersion\Uninstall", true))
            {
                if (key != null)
                    key.DeleteSubKeyTree(AppName, false);
            }
        }
        catch { }

        // Delete files except running Uninstall.exe — schedule after exit via cmd
        string cmd = Path.Combine(Path.GetTempPath(), "upPlayer-uninst-" + Guid.NewGuid().ToString("N") + ".cmd");
        File.WriteAllText(cmd,
            "@echo off\r\n" +
            "ping 127.0.0.1 -n 2 >nul\r\n" +
            "rmdir /s /q \"" + installDir + "\"\r\n" +
            "del \"%~f0\"\r\n");
        Process.Start(new ProcessStartInfo
        {
            FileName = cmd,
            CreateNoWindow = true,
            UseShellExecute = false,
            WindowStyle = ProcessWindowStyle.Hidden
        });
        return 0;
    }

    private static void WriteUninstallCmd(string installDir)
    {
        // Last-resort uninstaller if Uninstall.exe was not packaged.
        string path = Path.Combine(installDir, "uninstall.cmd");
        File.WriteAllText(path,
            "@echo off\r\n" +
            "echo Uninstall.exe missing. Remove folder manually: \"" + installDir + "\"\r\n" +
            "pause\r\n");
    }

    private static void CopyDirectory(string source, string dest, ProgressFn progress)
    {
        foreach (string dir in Directory.GetDirectories(source, "*", SearchOption.AllDirectories))
        {
            string rel = dir.Substring(source.Length).TrimStart('\\', '/');
            Directory.CreateDirectory(Path.Combine(dest, rel));
        }

        string[] files = Directory.GetFiles(source, "*", SearchOption.AllDirectories);
        int copied = 0;
        for (int i = 0; i < files.Length; i++)
        {
            string file = files[i];
            string name = Path.GetFileName(file);
            if (string.Equals(name, "install-payload.ps1", StringComparison.OrdinalIgnoreCase) ||
                string.Equals(name, "install.cmd", StringComparison.OrdinalIgnoreCase) ||
                string.Equals(name, "payload.zip", StringComparison.OrdinalIgnoreCase) ||
                string.Equals(Path.GetExtension(name), ".zip", StringComparison.OrdinalIgnoreCase))
                continue;

            string rel = file.Substring(source.Length).TrimStart('\\', '/');
            string target = Path.Combine(dest, rel);
            Directory.CreateDirectory(Path.GetDirectoryName(target));
            File.Copy(file, target, true);
            copied++;

            // Map file copy into 35%..75% of overall progress.
            int pct = 35 + (int)(40.0 * (i + 1) / Math.Max(1, files.Length));
            if (progress != null)
                progress(pct, "Installing files...", rel);

            // Keep UI responsive and make progress visible even on fast disks.
            if ((copied % 3) == 0)
                Thread.Sleep(15);
        }
    }

    private static void CreateStartMenuShortcut(string exe, string installDir)
    {
        string programs = Environment.GetFolderPath(Environment.SpecialFolder.CommonPrograms);
        string folder = Path.Combine(programs, AppName);
        Directory.CreateDirectory(folder);
        string lnk = Path.Combine(folder, AppName + ".lnk");

        Type shellType = Type.GetTypeFromProgID("WScript.Shell");
        object shell = Activator.CreateInstance(shellType);
        object shortcut = shellType.InvokeMember("CreateShortcut", BindingFlags.InvokeMethod, null, shell, new object[] { lnk });
        Type scType = shortcut.GetType();
        scType.InvokeMember("TargetPath", BindingFlags.SetProperty, null, shortcut, new object[] { exe });
        scType.InvokeMember("WorkingDirectory", BindingFlags.SetProperty, null, shortcut, new object[] { installDir });
        scType.InvokeMember("IconLocation", BindingFlags.SetProperty, null, shortcut, new object[] { exe + ",0" });
        scType.InvokeMember("Save", BindingFlags.InvokeMethod, null, shortcut, null);

        string uninst = Path.Combine(installDir, "Uninstall.exe");
        if (File.Exists(uninst))
        {
            string ulnk = Path.Combine(folder, "Uninstall.lnk");
            object ushortcut = shellType.InvokeMember("CreateShortcut", BindingFlags.InvokeMethod, null, shell, new object[] { ulnk });
            Type uType = ushortcut.GetType();
            uType.InvokeMember("TargetPath", BindingFlags.SetProperty, null, ushortcut, new object[] { uninst });
            uType.InvokeMember("Arguments", BindingFlags.SetProperty, null, ushortcut, new object[] { "--uninstall --dir \"" + installDir + "\"" });
            uType.InvokeMember("WorkingDirectory", BindingFlags.SetProperty, null, ushortcut, new object[] { installDir });
            uType.InvokeMember("Save", BindingFlags.InvokeMethod, null, ushortcut, null);
        }
    }

    private static void RemoveStartMenuShortcut()
    {
        string folder = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.CommonPrograms), AppName);
        if (Directory.Exists(folder))
            Directory.Delete(folder, true);
    }

    private static void WriteUninstallRegistry(string installDir, string exe, string version)
    {
        string uninst = Path.Combine(installDir, "Uninstall.exe");
        string uninstallString = File.Exists(uninst)
            ? "\"" + uninst + "\" --uninstall --dir \"" + installDir + "\""
            : "cmd.exe /c rmdir /s /q \"" + installDir + "\"";

        using (RegistryKey key = Registry.LocalMachine.CreateSubKey(
                   @"Software\Microsoft\Windows\CurrentVersion\Uninstall\" + AppName))
        {
            key.SetValue("DisplayName", AppName + " " + version);
            key.SetValue("DisplayVersion", version);
            key.SetValue("Publisher", AppName);
            key.SetValue("InstallLocation", installDir);
            key.SetValue("DisplayIcon", exe);
            key.SetValue("UninstallString", uninstallString);
            key.SetValue("NoModify", 1, RegistryValueKind.DWord);
            key.SetValue("NoRepair", 1, RegistryValueKind.DWord);
            long size = 0;
            foreach (string f in Directory.GetFiles(installDir, "*", SearchOption.AllDirectories))
                size += new FileInfo(f).Length;
            key.SetValue("EstimatedSize", (int)(size / 1024), RegistryValueKind.DWord);
        }
    }

    private static void RegisterAssociations(string exe)
    {
        string cmd = "\"" + exe + "\" \"%1\"";
        string icon = "\"" + exe + "\",0";
        string exeName = Path.GetFileName(exe);

        using (RegistryKey appPaths = Registry.LocalMachine.CreateSubKey(
                   @"Software\Microsoft\Windows\CurrentVersion\App Paths\" + exeName))
        {
            appPaths.SetValue("", exe);
            appPaths.SetValue("Path", Path.GetDirectoryName(exe));
        }

        using (RegistryKey app = Registry.LocalMachine.CreateSubKey(@"Software\Classes\Applications\" + exeName))
        {
            app.SetValue("FriendlyAppName", AppName);
            using (RegistryKey open = app.CreateSubKey(@"shell\open"))
            {
                open.SetValue("", "Play with " + AppName);
                using (RegistryKey c = open.CreateSubKey("command"))
                    c.SetValue("", cmd);
            }
            using (RegistryKey types = app.CreateSubKey("SupportedTypes"))
            {
                foreach (string ext in Extensions)
                    types.SetValue(ext, "");
            }
        }

        using (RegistryKey cap = Registry.LocalMachine.CreateSubKey(@"Software\Clients\Media\" + AppName + @"\Capabilities"))
        {
            cap.SetValue("ApplicationName", AppName);
            cap.SetValue("ApplicationDescription", "Lightweight Win32 front-end for mpv");
            using (RegistryKey fa = cap.CreateSubKey("FileAssociations"))
            {
                foreach (string ext in Extensions)
                {
                    string progId = AppName + ext;
                    fa.SetValue(ext, progId);

                    using (RegistryKey prog = Registry.LocalMachine.CreateSubKey(@"Software\Classes\" + progId))
                    {
                        prog.SetValue("", AppName + " Video");
                        using (RegistryKey di = prog.CreateSubKey("DefaultIcon"))
                            di.SetValue("", icon);
                        using (RegistryKey open = prog.CreateSubKey(@"shell\open"))
                        {
                            open.SetValue("", "Play with " + AppName);
                            open.SetValue("MultiSelectModel", "Player");
                            using (RegistryKey c = open.CreateSubKey("command"))
                                c.SetValue("", cmd);
                        }
                    }

                    using (RegistryKey owp = Registry.LocalMachine.CreateSubKey(@"Software\Classes\" + ext + @"\OpenWithProgids"))
                        owp.SetValue(progId, new byte[0], RegistryValueKind.None);
                    Registry.LocalMachine.CreateSubKey(@"Software\Classes\" + ext + @"\OpenWithList\" + exeName);
                }
            }
        }

        using (RegistryKey ra = Registry.LocalMachine.CreateSubKey(@"Software\RegisteredApplications"))
            ra.SetValue(AppName, @"Software\Clients\Media\" + AppName + @"\Capabilities");

        try
        {
            NativeMethods.SHChangeNotify(0x08000000, 0, IntPtr.Zero, IntPtr.Zero);
        }
        catch { }
    }

    private static void UnregisterAssociations(string exe)
    {
        string exeName = string.IsNullOrEmpty(exe) ? ExeName : Path.GetFileName(exe);
        TryDeleteKey(Registry.LocalMachine, @"Software\Microsoft\Windows\CurrentVersion\App Paths\" + exeName);
        TryDeleteKey(Registry.LocalMachine, @"Software\Classes\Applications\" + exeName);
        TryDeleteKey(Registry.LocalMachine, @"Software\Clients\Media\" + AppName);
        try
        {
            using (RegistryKey ra = Registry.LocalMachine.OpenSubKey(@"Software\RegisteredApplications", true))
            {
                if (ra != null)
                    ra.DeleteValue(AppName, false);
            }
        }
        catch { }

        foreach (string ext in Extensions)
        {
            string progId = AppName + ext;
            TryDeleteKey(Registry.LocalMachine, @"Software\Classes\" + progId);
            try
            {
                using (RegistryKey owp = Registry.LocalMachine.OpenSubKey(@"Software\Classes\" + ext + @"\OpenWithProgids", true))
                {
                    if (owp != null)
                        owp.DeleteValue(progId, false);
                }
            }
            catch { }
            TryDeleteKey(Registry.LocalMachine, @"Software\Classes\" + ext + @"\OpenWithList\" + exeName);
        }

        try
        {
            NativeMethods.SHChangeNotify(0x08000000, 0, IntPtr.Zero, IntPtr.Zero);
        }
        catch { }
    }

    private static void TryDeleteKey(RegistryKey root, string path)
    {
        try { root.DeleteSubKeyTree(path, false); } catch { }
    }

    private static void ExtractPayload(string selfPath, string zipPath)
    {
        using (var fs = new FileStream(selfPath, FileMode.Open, FileAccess.Read, FileShare.Read))
        {
            if (fs.Length < 16)
                throw new InvalidDataException("Setup package is truncated.");

            fs.Seek(-16, SeekOrigin.End);
            byte[] trailer = new byte[16];
            if (fs.Read(trailer, 0, 16) != 16)
                throw new InvalidDataException("Unable to read setup trailer.");

            long zipOffset = BitConverter.ToInt64(trailer, 0);
            long magic = BitConverter.ToInt64(trailer, 8);
            if (magic != Magic || zipOffset <= 0 || zipOffset >= fs.Length - 16)
                throw new InvalidDataException("Setup payload marker not found. Rebuild with package-setup.ps1.");

            long zipLength = fs.Length - 16 - zipOffset;
            if (zipLength <= 0)
                throw new InvalidDataException("Setup zip payload is empty.");

            fs.Seek(zipOffset, SeekOrigin.Begin);
            using (var outFs = new FileStream(zipPath, FileMode.Create, FileAccess.Write))
            {
                byte[] buffer = new byte[1024 * 128];
                long remaining = zipLength;
                while (remaining > 0)
                {
                    int toRead = (int)Math.Min(buffer.Length, remaining);
                    int n = fs.Read(buffer, 0, toRead);
                    if (n <= 0)
                        throw new EndOfStreamException("Unexpected end of setup payload.");
                    outFs.Write(buffer, 0, n);
                    remaining -= n;
                }
            }
        }
    }

    private sealed class SetupWizard : Form
    {
        private readonly string _version;
        private Panel _pageDir;
        private Panel _pageProgress;
        private Panel _pageDone;
        private TextBox _pathBox;
        private Label _statusLabel;
        private Label _detailLabel;
        private ProgressBar _bar;
        private ListBox _log;
        private Button _installBtn;
        private Button _cancelBtn;
        private Button _finishBtn;
        private CheckBox _launchBox;
        private string _installedExe = "";
        private bool _busy;

        public int ExitCode { get; private set; }

        public SetupWizard(string version)
        {
            _version = version;
            ExitCode = 1;

            Text = "upPlayer Setup";
            FormBorderStyle = FormBorderStyle.FixedDialog;
            MaximizeBox = false;
            MinimizeBox = false;
            StartPosition = FormStartPosition.CenterScreen;
            ClientSize = new Size(520, 340);
            Font = new Font("Segoe UI", 9F);
            try
            {
                string self = Application.ExecutablePath;
                if (!string.IsNullOrEmpty(self) && File.Exists(self))
                    Icon = Icon.ExtractAssociatedIcon(self);
            }
            catch { }

            _pageDir = BuildDirPage();
            _pageProgress = BuildProgressPage();
            _pageDone = BuildDonePage();
            Controls.Add(_pageDir);
            Controls.Add(_pageProgress);
            Controls.Add(_pageDone);
            ShowPage(_pageDir);

            FormClosing += delegate(object sender, FormClosingEventArgs e)
            {
                if (_busy)
                    e.Cancel = true;
            };
        }

        private Panel BuildDirPage()
        {
            var page = new Panel { Dock = DockStyle.Fill };

            page.Controls.Add(new Label
            {
                AutoSize = false,
                Location = new Point(20, 18),
                Size = new Size(480, 28),
                Text = "Welcome to upPlayer " + _version + " Setup",
                Font = new Font("Segoe UI", 12F, FontStyle.Bold)
            });
            page.Controls.Add(new Label
            {
                AutoSize = false,
                Location = new Point(20, 56),
                Size = new Size(480, 40),
                Text = "This will install upPlayer on your computer. Choose the install folder, then click Install."
            });
            page.Controls.Add(new Label
            {
                AutoSize = true,
                Location = new Point(20, 110),
                Text = "Install folder:"
            });

            _pathBox = new TextBox
            {
                Location = new Point(20, 132),
                Size = new Size(390, 23),
                Text = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.ProgramFiles), AppName)
            };
            page.Controls.Add(_pathBox);

            var browse = new Button
            {
                Location = new Point(420, 130),
                Size = new Size(80, 26),
                Text = "Browse..."
            };
            browse.Click += delegate
            {
                using (var dlg = new FolderBrowserDialog())
                {
                    dlg.Description = "Choose install folder";
                    dlg.SelectedPath = Directory.Exists(_pathBox.Text)
                        ? _pathBox.Text
                        : Environment.GetFolderPath(Environment.SpecialFolder.ProgramFiles);
                    if (dlg.ShowDialog(this) == DialogResult.OK)
                        _pathBox.Text = dlg.SelectedPath;
                }
            };
            page.Controls.Add(browse);

            _installBtn = new Button
            {
                Location = new Point(320, 290),
                Size = new Size(90, 28),
                Text = "Install"
            };
            _installBtn.Click += delegate { StartInstall(); };
            page.Controls.Add(_installBtn);

            _cancelBtn = new Button
            {
                Location = new Point(420, 290),
                Size = new Size(80, 28),
                Text = "Cancel"
            };
            _cancelBtn.Click += delegate { Close(); };
            page.Controls.Add(_cancelBtn);

            AcceptButton = _installBtn;
            CancelButton = _cancelBtn;
            return page;
        }

        private Panel BuildProgressPage()
        {
            var page = new Panel { Dock = DockStyle.Fill, Visible = false };

            page.Controls.Add(new Label
            {
                AutoSize = false,
                Location = new Point(20, 18),
                Size = new Size(480, 28),
                Text = "Installing upPlayer",
                Font = new Font("Segoe UI", 12F, FontStyle.Bold)
            });

            _statusLabel = new Label
            {
                AutoSize = false,
                Location = new Point(20, 60),
                Size = new Size(480, 22),
                Text = "Please wait..."
            };
            page.Controls.Add(_statusLabel);

            _detailLabel = new Label
            {
                AutoSize = false,
                Location = new Point(20, 84),
                Size = new Size(480, 20),
                ForeColor = Color.DimGray,
                Text = ""
            };
            page.Controls.Add(_detailLabel);

            _bar = new ProgressBar
            {
                Location = new Point(20, 118),
                Size = new Size(480, 22),
                Minimum = 0,
                Maximum = 100,
                Style = ProgressBarStyle.Continuous
            };
            page.Controls.Add(_bar);

            page.Controls.Add(new Label
            {
                AutoSize = true,
                Location = new Point(20, 156),
                Text = "Details:"
            });

            _log = new ListBox
            {
                Location = new Point(20, 178),
                Size = new Size(480, 140),
                IntegralHeight = false
            };
            page.Controls.Add(_log);
            return page;
        }

        private Panel BuildDonePage()
        {
            var page = new Panel { Dock = DockStyle.Fill, Visible = false };

            page.Controls.Add(new Label
            {
                AutoSize = false,
                Location = new Point(20, 18),
                Size = new Size(480, 28),
                Text = "Installation complete",
                Font = new Font("Segoe UI", 12F, FontStyle.Bold)
            });
            page.Controls.Add(new Label
            {
                AutoSize = false,
                Location = new Point(20, 60),
                Size = new Size(480, 40),
                Text = "upPlayer " + _version + " has been installed on your computer."
            });

            _launchBox = new CheckBox
            {
                AutoSize = true,
                Location = new Point(20, 120),
                Checked = true,
                Text = "Start upPlayer"
            };
            page.Controls.Add(_launchBox);

            _finishBtn = new Button
            {
                Location = new Point(420, 290),
                Size = new Size(80, 28),
                Text = "Finish"
            };
            _finishBtn.Click += delegate
            {
                bool launch = _launchBox.Checked;
                string exe = _installedExe;
                ExitCode = 0;
                Close();
                // Setup is elevated; start the app unelevated or mpv IPC/--wid can fail.
                if (launch && !string.IsNullOrEmpty(exe) && File.Exists(exe))
                    LaunchUnelevated(exe);
            };
            page.Controls.Add(_finishBtn);
            return page;
        }

        private void ShowPage(Panel page)
        {
            _pageDir.Visible = page == _pageDir;
            _pageProgress.Visible = page == _pageProgress;
            _pageDone.Visible = page == _pageDone;
        }

        private void StartInstall()
        {
            string path = _pathBox.Text.Trim();
            if (string.IsNullOrEmpty(path))
            {
                MessageBox.Show(this, "Please choose an install folder.", Text, MessageBoxButtons.OK, MessageBoxIcon.Warning);
                return;
            }
            string name = Path.GetFileName(path.TrimEnd('\\', '/'));
            if (!string.Equals(name, AppName, StringComparison.OrdinalIgnoreCase))
                path = Path.Combine(path, AppName);
            _pathBox.Text = path;

            _busy = true;
            _installBtn.Enabled = false;
            _cancelBtn.Enabled = false;
            ShowPage(_pageProgress);
            Report(0, "Starting...", path);

            var worker = new BackgroundWorker();
            worker.WorkerReportsProgress = true;
            worker.DoWork += delegate(object sender, DoWorkEventArgs e)
            {
                e.Result = InstallCore(path, _version, delegate(int percent, string status, string detail)
                {
                    worker.ReportProgress(percent, status + "\n" + detail);
                });
            };
            worker.ProgressChanged += delegate(object sender, ProgressChangedEventArgs e)
            {
                string text = e.UserState as string ?? "";
                string[] parts = text.Split(new char[] { '\n' }, 2);
                Report(e.ProgressPercentage, parts.Length > 0 ? parts[0] : "", parts.Length > 1 ? parts[1] : "");
            };
            worker.RunWorkerCompleted += delegate(object sender, RunWorkerCompletedEventArgs e)
            {
                _busy = false;
                if (e.Error != null)
                {
                    MessageBox.Show(this, e.Error.Message, Text, MessageBoxButtons.OK, MessageBoxIcon.Error);
                    ShowPage(_pageDir);
                    _installBtn.Enabled = true;
                    _cancelBtn.Enabled = true;
                    return;
                }
                _installedExe = e.Result as string ?? "";
                ShowPage(_pageDone);
                AcceptButton = _finishBtn;
            };
            worker.RunWorkerAsync();
        }

        private void Report(int percent, string status, string detail)
        {
            if (percent < 0) percent = 0;
            if (percent > 100) percent = 100;
            _bar.Value = percent;
            _statusLabel.Text = status ?? "";
            _detailLabel.Text = detail ?? "";
            if (!string.IsNullOrEmpty(detail))
            {
                _log.Items.Add(detail);
                _log.TopIndex = Math.Max(0, _log.Items.Count - 1);
            }
        }
    }

    private static class NativeMethods
    {
        [System.Runtime.InteropServices.DllImport("shell32.dll")]
        public static extern void SHChangeNotify(int wEventId, uint uFlags, IntPtr dwItem1, IntPtr dwItem2);
    }
}
