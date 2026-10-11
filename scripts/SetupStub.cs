// Self-extracting setup stub. Binary layout: [stub.exe][zip payload][Int64 zipOffset][Int64 magic]
using System;
using System.Diagnostics;
using System.IO;
using System.IO.Compression;
using System.Reflection;
using System.Text;
using System.Windows.Forms;

internal static class Program
{
    private const long Magic = 0x5550504C53544658; // "UPPLSTFX"

    [STAThread]
    private static int Main(string[] args)
    {
        try
        {
            string self = Assembly.GetExecutingAssembly().Location;
            if (string.IsNullOrEmpty(self))
                self = Process.GetCurrentProcess().MainModule.FileName;

            string version = "1.0.0";
            for (int i = 0; i < args.Length - 1; i++)
            {
                if (args[i] == "--version")
                    version = args[i + 1];
            }

            if (MessageBox.Show(
                    "Install upPlayer " + version + " for all users?\n\nFiles go to:\n" +
                    Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.ProgramFiles), "upPlayer") +
                    "\n\nAdministrator permission is required.",
                    "upPlayer Setup",
                    MessageBoxButtons.OKCancel,
                    MessageBoxIcon.Information) != DialogResult.OK)
            {
                return 1;
            }

            string tempRoot = Path.Combine(Path.GetTempPath(), "upPlayer-setup-" + Guid.NewGuid().ToString("N"));
            Directory.CreateDirectory(tempRoot);
            string zipPath = Path.Combine(tempRoot, "payload.zip");

            ExtractPayload(self, zipPath);
            ZipFile.ExtractToDirectory(zipPath, tempRoot);

            string script = Path.Combine(tempRoot, "install-payload.ps1");
            if (!File.Exists(script))
                throw new FileNotFoundException("install-payload.ps1 missing from setup payload.");

            var psi = new ProcessStartInfo
            {
                FileName = "powershell.exe",
                Arguments = "-NoProfile -ExecutionPolicy Bypass -File \"" + script + "\" -AppVersion \"" + version + "\"",
                UseShellExecute = false,
                WorkingDirectory = tempRoot
            };
            using (var proc = Process.Start(psi))
            {
                proc.WaitForExit();
                if (proc.ExitCode != 0)
                    throw new Exception("Installer script failed with exit code " + proc.ExitCode);
            }

            try { Directory.Delete(tempRoot, true); } catch { }
            return 0;
        }
        catch (Exception ex)
        {
            MessageBox.Show(ex.Message, "upPlayer Setup", MessageBoxButtons.OK, MessageBoxIcon.Error);
            return 1;
        }
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
}
