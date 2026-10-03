# Copy TCP connections from this PC's LAN addresses to vapord on localhost.
# Started by start-wsl-server.ps1. netsh portproxy resets those connections.
param(
    [int]$Port = 8777,
    [string]$Target = '127.0.0.1',
    [int]$TargetPort = 8777
)

$ErrorActionPreference = 'Stop'

Add-Type -TypeDefinition @'
using System;
using System.Net;
using System.Net.Sockets;
using System.Threading;

public static class VaporLanForward {
    public static void Run(string ips, int port, string target, int targetPort) {
        string[] list = ips.Split(new char[] {','}, StringSplitOptions.RemoveEmptyEntries);
        foreach (string ip in list) {
            TcpListener listener = new TcpListener(IPAddress.Parse(ip), port);
            listener.Server.SetSocketOption(SocketOptionLevel.Socket, SocketOptionName.ReuseAddress, true);
            listener.Start();
            ThreadPool.QueueUserWorkItem(delegate(object state) {
                TcpListener accept = (TcpListener)state;
                while (true) {
                    TcpClient client = null;
                    try {
                        client = accept.AcceptTcpClient();
                    } catch (Exception) {
                        break;
                    }
                    ThreadPool.QueueUserWorkItem(delegate(object cstate) {
                        TcpClient from = (TcpClient)cstate;
                        TcpClient to = null;
                        try {
                            to = new TcpClient();
                            to.Connect(target, targetPort);
                            Thread a = new Thread(delegate() { Copy(from, to); });
                            Thread b = new Thread(delegate() { Copy(to, from); });
                            a.IsBackground = true;
                            b.IsBackground = true;
                            a.Start();
                            b.Start();
                            a.Join();
                            b.Join();
                        } catch (Exception) {
                        } finally {
                            if (to != null) to.Close();
                            from.Close();
                        }
                    }, client);
                }
            }, listener);
        }
        Thread.Sleep(Timeout.Infinite);
    }

    static void Copy(TcpClient src, TcpClient dst) {
        try {
            byte[] buf = new byte[65536];
            NetworkStream inn = src.GetStream();
            NetworkStream outn = dst.GetStream();
            int n;
            while ((n = inn.Read(buf, 0, buf.Length)) > 0) {
                outn.Write(buf, 0, n);
            }
        } catch (Exception) {
        }
        try { dst.Client.Shutdown(SocketShutdown.Send); } catch (Exception) {}
    }
}
'@

$ips = @(Get-NetIPAddress -AddressFamily IPv4 | Where-Object {
        $_.IPAddress -notlike '127.*' -and
        $_.IPAddress -notlike '169.254.*' -and
        $_.InterfaceAlias -notlike 'vEthernet*'
    } | Select-Object -ExpandProperty IPAddress -Unique)

if ($ips.Count -eq 0) {
    throw "no LAN address to listen on"
}

$deadline = (Get-Date).AddSeconds(8)
while ($true) {
    try {
        [VaporLanForward]::Run(($ips -join ','), $Port, $Target, $TargetPort)
        break
    } catch {
        if ((Get-Date) -gt $deadline) { throw }
        Start-Sleep -Milliseconds 300
    }
}
