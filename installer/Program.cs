using System;
using System.IO;
using System.IO.Compression;
using System.Reflection;
using System.Diagnostics;
using System.Threading.Tasks;
using System.Drawing;
using System.Windows.Forms;
using Microsoft.Win32;

namespace AgOpenGpsSuiteInstaller
{
    static class Program
    {
        [STAThread]
        static void Main()
        {
            Application.EnableVisualStyles();
            Application.SetCompatibleTextRenderingDefault(false);
            Application.Run(new MainForm());
        }
    }

    public class MainForm : Form
    {
        private TextBox txtPath;
        private Button btnBrowse;
        private CheckBox chkAgOpen;
        private CheckBox chkTc;
        private CheckBox chkTerminal;
        private CheckBox chkShortcuts;
        private CheckBox chkRunAllShortcut;
        private CheckBox chkFirewall;
        private ProgressBar progressBar;
        private Label lblStatus;
        private Button btnInstall;
        private Button btnCancel;
        private CheckBox chkLaunchAfter;

        public MainForm()
        {
            InitializeComponent();
        }

        private void InitializeComponent()
        {
            this.Text = "AG-ISOBUS PRO 1200 & AgOpenGPS Precision Suite - Instalator";
            this.Size = new Size(650, 560);
            this.StartPosition = FormStartPosition.CenterScreen;
            this.FormBorderStyle = FormBorderStyle.FixedDialog;
            this.MaximizeBox = false;
            this.BackColor = Color.FromArgb(24, 28, 36);
            this.ForeColor = Color.FromArgb(235, 240, 245);
            this.Font = new Font("Segoe UI", 9.5f, FontStyle.Regular);

            // Header Banner
            var pnlHeader = new Panel
            {
                Dock = DockStyle.Top,
                Height = 85,
                BackColor = Color.FromArgb(16, 20, 28)
            };
            var lblTitle = new Label
            {
                Text = "🚜 AG-ISOBUS PRO 1200 & AgOpenGPS Precision Suite",
                Font = new Font("Segoe UI", 13.5f, FontStyle.Bold),
                ForeColor = Color.FromArgb(46, 204, 113),
                Location = new Point(18, 14),
                AutoSize = true
            };
            var lblSub = new Label
            {
                Text = "Kompletny pakiet: Nawigacja polowa RTK, Kontroler Zadań TC i Terminal ISOBUS z Web-VT",
                Font = new Font("Segoe UI", 8.8f, FontStyle.Regular),
                ForeColor = Color.FromArgb(160, 175, 195),
                Location = new Point(20, 48),
                AutoSize = true
            };
            pnlHeader.Controls.Add(lblTitle);
            pnlHeader.Controls.Add(lblSub);
            this.Controls.Add(pnlHeader);

            // Group: Destination
            var grpDest = new GroupBox
            {
                Text = " Katalog instalacji ",
                ForeColor = Color.FromArgb(120, 190, 255),
                Location = new Point(20, 100),
                Size = new Size(595, 75)
            };
            txtPath = new TextBox
            {
                Text = @"C:\AgOpenGPS_Suite",
                Location = new Point(15, 28),
                Size = new Size(455, 26),
                BackColor = Color.FromArgb(34, 40, 52),
                ForeColor = Color.White
            };
            btnBrowse = new Button
            {
                Text = "Przeglądaj...",
                Location = new Point(480, 26),
                Size = new Size(100, 29),
                BackColor = Color.FromArgb(45, 54, 70),
                ForeColor = Color.White,
                FlatStyle = FlatStyle.Flat
            };
            btnBrowse.Click += (s, e) =>
            {
                using var fbd = new FolderBrowserDialog { SelectedPath = txtPath.Text };
                if (fbd.ShowDialog() == DialogResult.OK) txtPath.Text = fbd.SelectedPath;
            };
            grpDest.Controls.Add(txtPath);
            grpDest.Controls.Add(btnBrowse);
            this.Controls.Add(grpDest);

            // Group: Components
            var grpComp = new GroupBox
            {
                Text = " Składniki do zainstalowania ",
                ForeColor = Color.FromArgb(120, 190, 255),
                Location = new Point(20, 185),
                Size = new Size(595, 175)
            };
            chkAgOpen = new CheckBox
            {
                Text = "AgOpenGPS + AgIO (Nawigacja polowa, auto-steer, kontrola sekcji)",
                Checked = true,
                Location = new Point(18, 26),
                Size = new Size(550, 24),
                ForeColor = Color.White
            };
            chkTc = new CheckBox
            {
                Text = "AOG-TaskController (Mostek sekcji ISOBUS ISO 11783-10)",
                Checked = true,
                Location = new Point(18, 52),
                Size = new Size(550, 24),
                ForeColor = Color.White
            };
            chkTerminal = new CheckBox
            {
                Text = "AG-ISOBUS PRO 1200 Terminal (Wirtualny Terminal VT v2..v6 + Web-VT na tablet)",
                Checked = true,
                Location = new Point(18, 78),
                Size = new Size(550, 24),
                ForeColor = Color.White
            };
            chkShortcuts = new CheckBox
            {
                Text = "Utwórz skróty na Pulpicie i w Menu Start",
                Checked = true,
                Location = new Point(18, 108),
                Size = new Size(550, 24),
                ForeColor = Color.FromArgb(200, 220, 240)
            };
            chkRunAllShortcut = new CheckBox
            {
                Text = "Utwórz skrót '🚀 URUCHOM CAŁY ZESTAW' (1-kliknięcie odpala AgIO + AOG + TC + VT)",
                Checked = true,
                Location = new Point(18, 134),
                Size = new Size(550, 24),
                ForeColor = Color.FromArgb(46, 204, 113)
            };
            grpComp.Controls.Add(chkAgOpen);
            grpComp.Controls.Add(chkTc);
            grpComp.Controls.Add(chkTerminal);
            grpComp.Controls.Add(chkShortcuts);
            grpComp.Controls.Add(chkRunAllShortcut);
            this.Controls.Add(grpComp);

            // Group: Options
            chkFirewall = new CheckBox
            {
                Text = "Skonfiguruj Zaporę Windows Defender (zezwól na ruch UDP 8888, 9999 oraz Web-VT 8080)",
                Checked = true,
                Location = new Point(25, 368),
                Size = new Size(585, 24),
                ForeColor = Color.FromArgb(180, 200, 220)
            };
            this.Controls.Add(chkFirewall);

            // Progress & Status
            lblStatus = new Label
            {
                Text = "Gotowy do instalacji.",
                Location = new Point(22, 400),
                Size = new Size(590, 20),
                ForeColor = Color.FromArgb(160, 180, 205)
            };
            this.Controls.Add(lblStatus);

            progressBar = new ProgressBar
            {
                Location = new Point(22, 424),
                Size = new Size(590, 22),
                Style = ProgressBarStyle.Blocks,
                Value = 0
            };
            this.Controls.Add(progressBar);

            // Bottom Panel
            chkLaunchAfter = new CheckBox
            {
                Text = "Uruchom zestaw po zakończeniu instalacji",
                Checked = true,
                Location = new Point(22, 465),
                Size = new Size(320, 24),
                ForeColor = Color.FromArgb(46, 204, 113)
            };
            this.Controls.Add(chkLaunchAfter);

            btnInstall = new Button
            {
                Text = " Zainstaluj pakiet ",
                Location = new Point(350, 460),
                Size = new Size(150, 36),
                BackColor = Color.FromArgb(39, 174, 96),
                ForeColor = Color.White,
                Font = new Font("Segoe UI", 10.5f, FontStyle.Bold),
                FlatStyle = FlatStyle.Flat
            };
            btnInstall.Click += async (s, e) => await StartInstallation();
            this.Controls.Add(btnInstall);

            btnCancel = new Button
            {
                Text = "Anuluj",
                Location = new Point(512, 460),
                Size = new Size(100, 36),
                BackColor = Color.FromArgb(60, 70, 85),
                ForeColor = Color.White,
                FlatStyle = FlatStyle.Flat
            };
            btnCancel.Click += (s, e) => this.Close();
            this.Controls.Add(btnCancel);
        }

        private async Task StartInstallation()
        {
            string targetDir = txtPath.Text.Trim();
            if (string.IsNullOrEmpty(targetDir))
            {
                MessageBox.Show("Podaj poprawny katalog instalacji!", "Błąd", MessageBoxButtons.OK, MessageBoxIcon.Warning);
                return;
            }

            btnInstall.Enabled = false;
            btnBrowse.Enabled = false;
            txtPath.Enabled = false;

            try
            {
                lblStatus.Text = "Przygotowywanie katalogu docelowego...";
                progressBar.Value = 10;
                Directory.CreateDirectory(targetDir);

                await Task.Run(() =>
                {
                    // 1. Extract embedded payload.zip
                    var assembly = Assembly.GetExecutingAssembly();
                    string resourceName = null;
                    foreach (var name in assembly.GetManifestResourceNames())
                    {
                        if (name.EndsWith("payload.zip", StringComparison.OrdinalIgnoreCase))
                        {
                            resourceName = name;
                            break;
                        }
                    }

                    if (string.IsNullOrEmpty(resourceName))
                        throw new FileNotFoundException("Nie znaleziono wbudowanego archiwum pakietu instalacyjnego!");

                    using var stream = assembly.GetManifestResourceStream(resourceName);
                    using var archive = new ZipArchive(stream, ZipArchiveMode.Read);

                    int total = archive.Entries.Count;
                    int cur = 0;

                    foreach (var entry in archive.Entries)
                    {
                        cur++;
                        int percent = 10 + (int)((float)cur / total * 70);

                        // Check selection filters
                        if (!chkAgOpen.Checked && entry.FullName.StartsWith("AgOpenGPS", StringComparison.OrdinalIgnoreCase))
                            continue;
                        if (!chkTc.Checked && entry.FullName.StartsWith("TaskController", StringComparison.OrdinalIgnoreCase))
                            continue;
                        if (!chkTerminal.Checked && entry.FullName.StartsWith("TerminalVT", StringComparison.OrdinalIgnoreCase))
                            continue;

                        string destFile = Path.Combine(targetDir, entry.FullName);
                        if (string.IsNullOrEmpty(entry.Name))
                        {
                            Directory.CreateDirectory(destFile);
                        }
                        else
                        {
                            Directory.CreateDirectory(Path.GetDirectoryName(destFile));
                            entry.ExtractToFile(destFile, true);
                        }

                        if (cur % 5 == 0 || cur == total)
                        {
                            this.Invoke((Action)(() =>
                            {
                                progressBar.Value = Math.Min(percent, 85);
                                lblStatus.Text = $"Wypakowywanie: {entry.Name}";
                            }));
                        }
                    }

                    // 2. Configure Windows Firewall rules
                    if (chkFirewall.Checked)
                    {
                        this.Invoke((Action)(() => lblStatus.Text = "Konfigurowanie zapory Windows Firewall..."));
                        RunCmd("netsh", "advfirewall firewall add rule name=\"AgOpenGPS UDP Broadcast\" dir=in action=allow protocol=UDP localport=8888,9999,17777");
                        RunCmd("netsh", "advfirewall firewall add rule name=\"ISOBUS Web-VT Server\" dir=in action=allow protocol=TCP localport=8080");
                    }

                    // 3. Create Windows Shortcuts
                    if (chkShortcuts.Checked)
                    {
                        this.Invoke((Action)(() =>
                        {
                            progressBar.Value = 92;
                            lblStatus.Text = "Tworzenie skrótów na Pulpicie...";
                        }));

                        string desktop = Environment.GetFolderPath(Environment.SpecialFolder.DesktopDirectory);
                        string startMenu = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.Programs), "AG-ISOBUS Precision Suite");
                        Directory.CreateDirectory(startMenu);

                        if (chkTerminal.Checked)
                        {
                            string vtExe = Path.Combine(targetDir, "TerminalVT", "isobus_vt_terminal.exe");
                            CreateShortcut(Path.Combine(desktop, "AG-ISOBUS PRO 1200.lnk"), vtExe, Path.GetDirectoryName(vtExe), "Terminal ISOBUS Universal Terminal");
                            CreateShortcut(Path.Combine(startMenu, "AG-ISOBUS PRO 1200.lnk"), vtExe, Path.GetDirectoryName(vtExe), "Terminal ISOBUS Universal Terminal");
                        }

                        if (chkAgOpen.Checked)
                        {
                            string aogExe = Path.Combine(targetDir, "AgOpenGPS", "AgOpenGPS.exe");
                            CreateShortcut(Path.Combine(desktop, "AgOpenGPS.lnk"), aogExe, Path.GetDirectoryName(aogExe), "AgOpenGPS Nawigacja Polowa");
                            CreateShortcut(Path.Combine(startMenu, "AgOpenGPS.lnk"), aogExe, Path.GetDirectoryName(aogExe), "AgOpenGPS Nawigacja Polowa");
                        }

                        if (chkTc.Checked)
                        {
                            string tcExe = Path.Combine(targetDir, "TaskController", "AOG-TaskController.exe");
                            CreateShortcut(Path.Combine(desktop, "AOG Task Controller.lnk"), tcExe, Path.GetDirectoryName(tcExe), "Task Controller ISO 11783-10");
                            CreateShortcut(Path.Combine(startMenu, "AOG Task Controller.lnk"), tcExe, Path.GetDirectoryName(tcExe), "Task Controller ISO 11783-10");
                        }

                        if (chkRunAllShortcut.Checked)
                        {
                            string runAllBat = Path.Combine(targetDir, "Uruchom_Caly_Zestaw.bat");
                            CreateShortcut(Path.Combine(desktop, "🚀 URUCHOM CAŁY ZESTAW.lnk"), runAllBat, targetDir, "Uruchamia AgIO, AgOpenGPS, TaskController oraz Terminal VT");
                            CreateShortcut(Path.Combine(startMenu, "🚀 URUCHOM CAŁY ZESTAW.lnk"), runAllBat, targetDir, "Uruchamia AgIO, AgOpenGPS, TaskController oraz Terminal VT");
                        }
                    }

                    // 4. Create Uninstaller
                    CreateUninstaller(targetDir);
                });

                progressBar.Value = 100;
                lblStatus.Text = "Instalacja zakończona pełnym sukcesem!";
                btnInstall.Text = " Zakończ ";
                btnInstall.Enabled = true;
                btnCancel.Visible = false;

                btnInstall.Click -= null;
                btnInstall.Click += (s, e) =>
                {
                    if (chkLaunchAfter.Checked)
                    {
                        string runAllBat = Path.Combine(targetDir, "Uruchom_Caly_Zestaw.bat");
                        if (File.Exists(runAllBat))
                        {
                            Process.Start(new ProcessStartInfo(runAllBat) { WorkingDirectory = targetDir });
                        }
                    }
                    this.Close();
                };

                MessageBox.Show(
                    "Instalacja pakietu AG-ISOBUS PRO 1200 & AgOpenGPS zakończyła się sukcesem!\n\n" +
                    "Skróty do aplikacji zostały utworzone na Twoim Pulpicie.\n" +
                    "W kabinie ciągnika możesz połączyć tablet pod adresem:\n" +
                    "http://192.168.68.105:8080",
                    "Instalacja Zakończona",
                    MessageBoxButtons.OK,
                    MessageBoxIcon.Information
                );
            }
            catch (Exception ex)
            {
                lblStatus.Text = "Błąd instalacji!";
                MessageBox.Show($"Wystąpił błąd podczas instalacji:\n{ex.Message}", "Błąd", MessageBoxButtons.OK, MessageBoxIcon.Error);
                btnInstall.Enabled = true;
                btnBrowse.Enabled = true;
                txtPath.Enabled = true;
            }
        }

        private static void CreateShortcut(string shortcutPath, string targetPath, string workingDir, string description)
        {
            try
            {
                Type shellType = Type.GetTypeFromProgID("WScript.Shell");
                if (shellType == null) return;
                dynamic shell = Activator.CreateInstance(shellType);
                dynamic shortcut = shell.CreateShortcut(shortcutPath);
                shortcut.TargetPath = targetPath;
                shortcut.WorkingDirectory = workingDir;
                shortcut.Description = description;
                shortcut.Save();
            }
            catch { }
        }

        private static void RunCmd(string fileName, string args)
        {
            try
            {
                var psi = new ProcessStartInfo(fileName, args)
                {
                    CreateNoWindow = true,
                    UseShellExecute = false
                };
                using var p = Process.Start(psi);
                p?.WaitForExit(3000);
            }
            catch { }
        }

        private static void CreateUninstaller(string installDir)
        {
            try
            {
                string uninstallerBat = Path.Combine(installDir, "Odinstaluj.bat");
                string content = $@"@echo off
title Deinstalacja AG-ISOBUS Suite
color 0C
echo Czy na pewno chcesz usunac pakiet AG-ISOBUS PRO & AgOpenGPS?
echo Nacisnij dowolny klawisz, aby potwierdzic usuniecie...
pause >nul
cd /d C:\
rmdir /s /q ""{installDir}""
del /f /q ""%USERPROFILE%\Desktop\AG-ISOBUS PRO 1200.lnk"" 2>nul
del /f /q ""%USERPROFILE%\Desktop\AgOpenGPS.lnk"" 2>nul
del /f /q ""%USERPROFILE%\Desktop\AOG Task Controller.lnk"" 2>nul
del /f /q ""%USERPROFILE%\Desktop\🚀 URUCHOM CAŁY ZESTAW.lnk"" 2>nul
echo Pakiet zostal pomyslnie usuniety.
pause
";
                File.WriteAllText(uninstallerBat, content, System.Text.Encoding.GetEncoding(852));

                // Register in Windows Add/Remove Programs
                using var key = Registry.CurrentUser.CreateSubKey(@"Software\Microsoft\Windows\CurrentVersion\Uninstall\AgOpenGpsIsobusSuite");
                if (key != null)
                {
                    key.SetValue("DisplayName", "AG-ISOBUS PRO 1200 & AgOpenGPS Suite");
                    key.SetValue("DisplayVersion", "2.5.0");
                    key.SetValue("Publisher", "Precision Ag Open Solutions");
                    key.SetValue("UninstallString", $"\"{uninstallerBat}\"");
                    key.SetValue("InstallLocation", installDir);
                }
            }
            catch { }
        }
    }
}
