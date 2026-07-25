// QUIC counterpart to fuse_latbench: small-message latency while a bulk
// transfer saturates the link.
//
// Both the bulk stream and the probe stream run over ONE QUIC connection,
// which is the normal way to use QUIC and the thing being measured: QUIC's
// congestion control is connection-wide, so a saturating bulk stream and a
// latency-sensitive stream share one controller and one send queue. Fuse
// puts them on separate lanes with independent per-stream congestion state.
// The difference in probe latency between the two is the whole point.
//
// Timestamps use CLOCK_MONOTONIC directly rather than Instant, because the
// two ends are separate processes and Instant is not comparable across them.
//
//   quic_latbench recv <port> <cert-out>
//   quic_latbench send <host> <port> <cert-in> <seconds> <probe-hz> <loaded01>

use anyhow::{anyhow, Result};
use quinn::{ClientConfig, Endpoint, ServerConfig, TransportConfig};
use rustls::pki_types::{CertificateDer, PrivatePkcs8KeyDer};
use std::net::{Ipv4Addr, SocketAddr};
use std::sync::Arc;

const TAG_BULK: u8 = 1;
const TAG_PROBE: u8 = 2;
const PROBE_REC: usize = 16; // u64 nanos + u64 seq
const BULK_CHUNK: usize = 64 * 1024;

fn now_ns() -> u64 {
    let mut ts = libc::timespec { tv_sec: 0, tv_nsec: 0 };
    unsafe { libc::clock_gettime(libc::CLOCK_MONOTONIC, &mut ts) };
    ts.tv_sec as u64 * 1_000_000_000 + ts.tv_nsec as u64
}

fn transport() -> TransportConfig {
    let mut t = TransportConfig::default();
    t.stream_receive_window((16u32 * 1024 * 1024).into());
    t.receive_window((64u32 * 1024 * 1024).into());
    t.send_window(64 * 1024 * 1024);
    t
}

fn pct(v: &[u64], p: f64) -> f64 {
    if v.is_empty() {
        return 0.0;
    }
    v[((v.len() - 1) as f64 * p) as usize] as f64 / 1000.0
}

async fn run_server(port: u16, cert_path: String) -> Result<()> {
    let cert = rcgen::generate_simple_self_signed(vec!["localhost".into()])?;
    let cert_der = CertificateDer::from(cert.cert);
    let key = PrivatePkcs8KeyDer::from(cert.key_pair.serialize_der());
    std::fs::write(&cert_path, cert_der.as_ref())?;

    let mut sc = ServerConfig::with_single_cert(vec![cert_der], key.into())?;
    sc.transport_config(Arc::new(transport()));
    let endpoint = Endpoint::server(sc, SocketAddr::from((Ipv4Addr::UNSPECIFIED, port)))?;

    let conn = endpoint
        .accept()
        .await
        .ok_or_else(|| anyhow!("no connection"))?
        .await?;

    let lat: Arc<std::sync::Mutex<Vec<u64>>> = Arc::new(std::sync::Mutex::new(Vec::new()));
    let bulk_bytes = Arc::new(std::sync::atomic::AtomicU64::new(0));
    let mut tasks = Vec::new();

    // Streams are self-describing: each opens with a one-byte tag, so the
    // server does not depend on accept ordering.
    while let Ok(mut recv) = conn.accept_uni().await {
        let lat = lat.clone();
        let bulk_bytes = bulk_bytes.clone();
        tasks.push(tokio::spawn(async move {
            let mut tag = [0u8; 1];
            if recv.read_exact(&mut tag).await.is_err() {
                return;
            }
            if tag[0] == TAG_PROBE {
                let mut rec = [0u8; PROBE_REC];
                while recv.read_exact(&mut rec).await.is_ok() {
                    let sent = u64::from_be_bytes(rec[0..8].try_into().unwrap());
                    let t = now_ns();
                    if t > sent {
                        lat.lock().unwrap().push(t - sent);
                    }
                }
            } else {
                let mut buf = vec![0u8; BULK_CHUNK];
                while let Ok(Some(n)) = recv.read(&mut buf).await {
                    bulk_bytes.fetch_add(n as u64, std::sync::atomic::Ordering::Relaxed);
                }
            }
        }));
    }
    for t in tasks {
        let _ = t.await;
    }

    let mut v = lat.lock().unwrap().clone();
    v.sort_unstable();
    println!(
        "RECV bulk_goodput_bytes={} probes={}",
        bulk_bytes.load(std::sync::atomic::Ordering::Relaxed),
        v.len()
    );
    println!(
        "probe-latency(us)      n={:<7} p50={:7.1}  p90={:7.1}  p99={:7.1}  p99.9={:7.1}  max={:8.1}",
        v.len(),
        pct(&v, 0.50),
        pct(&v, 0.90),
        pct(&v, 0.99),
        pct(&v, 0.999),
        v.last().copied().unwrap_or(0) as f64 / 1000.0
    );
    Ok(())
}

async fn run_client(
    host: String,
    port: u16,
    cert_path: String,
    seconds: f64,
    hz: u64,
    loaded: bool,
) -> Result<()> {
    let cert = CertificateDer::from(std::fs::read(&cert_path)?);
    let mut roots = rustls::RootCertStore::empty();
    roots.add(cert)?;
    let mut cc = ClientConfig::with_root_certificates(Arc::new(roots))?;
    cc.transport_config(Arc::new(transport()));

    let mut endpoint = Endpoint::client(SocketAddr::from((Ipv4Addr::UNSPECIFIED, 0)))?;
    endpoint.set_default_client_config(cc);
    let addr: SocketAddr = format!("{host}:{port}").parse()?;
    let conn = endpoint.connect(addr, "localhost")?.await?;

    let stop = Arc::new(std::sync::atomic::AtomicBool::new(false));
    let mut tasks = Vec::new();

    if loaded {
        let c = conn.clone();
        let stop = stop.clone();
        tasks.push(tokio::spawn(async move {
            if let Ok(mut s) = c.open_uni().await {
                let _ = s.write_all(&[TAG_BULK]).await;
                let buf = vec![0xC3u8; BULK_CHUNK];
                while !stop.load(std::sync::atomic::Ordering::Relaxed) {
                    if s.write_all(&buf).await.is_err() {
                        break;
                    }
                }
                let _ = s.finish();
            }
        }));
    }

    let probe_sent = Arc::new(std::sync::atomic::AtomicU64::new(0));
    {
        let c = conn.clone();
        let stop = stop.clone();
        let probe_sent = probe_sent.clone();
        tasks.push(tokio::spawn(async move {
            if let Ok(mut s) = c.open_uni().await {
                let _ = s.write_all(&[TAG_PROBE]).await;
                let interval = std::time::Duration::from_nanos(1_000_000_000 / hz.max(1));
                let mut seq = 0u64;
                while !stop.load(std::sync::atomic::Ordering::Relaxed) {
                    let mut rec = [0u8; PROBE_REC];
                    rec[0..8].copy_from_slice(&now_ns().to_be_bytes());
                    rec[8..16].copy_from_slice(&seq.to_be_bytes());
                    if s.write_all(&rec).await.is_err() {
                        break;
                    }
                    seq += 1;
                    probe_sent.fetch_add(1, std::sync::atomic::Ordering::Relaxed);
                    tokio::time::sleep(interval).await;
                }
                let _ = s.finish();
            }
        }));
    }

    tokio::time::sleep(std::time::Duration::from_secs_f64(seconds)).await;
    stop.store(true, std::sync::atomic::Ordering::Relaxed);
    for t in tasks {
        let _ = t.await;
    }
    // Give the peer a moment to drain before tearing the connection down.
    tokio::time::sleep(std::time::Duration::from_millis(300)).await;
    conn.close(0u32.into(), b"done");
    endpoint.wait_idle().await;

    println!(
        "SEND phase={} probes_sent={}",
        if loaded { "loaded" } else { "idle" },
        probe_sent.load(std::sync::atomic::Ordering::Relaxed)
    );
    Ok(())
}

#[tokio::main]
async fn main() -> Result<()> {
    rustls::crypto::ring::default_provider()
        .install_default()
        .map_err(|_| anyhow!("crypto provider"))?;
    let a: Vec<String> = std::env::args().collect();
    match a.get(1).map(|s| s.as_str()) {
        Some("recv") if a.len() >= 4 => run_server(a[2].parse()?, a[3].clone()).await,
        Some("send") if a.len() >= 8 => {
            run_client(
                a[2].clone(),
                a[3].parse()?,
                a[4].clone(),
                a[5].parse()?,
                a[6].parse()?,
                a[7] != "0",
            )
            .await
        }
        _ => {
            eprintln!(
                "usage:\n  {0} recv <port> <cert-out>\n  {0} send <host> <port> <cert-in> <seconds> <probe-hz> <loaded01>",
                a[0]
            );
            std::process::exit(2);
        }
    }
}
