// BLE Spectrum Monitor — chart setup and update helpers.
//
// Exposes a tiny API used by app.js:
//   createSpectrumChart(canvas) -> chart
//   updateSpectrumChart(chart, rssiArray)
//   createHeatmapChart(canvas)  -> chart
//   updateHeatmapChart(chart, history)
//
// `history` is an array of { t, rssi[40] } sweep records (oldest first).

export const NUM_CHANNELS = 40;
export const ADV_CHANNELS = new Set([37, 38, 39]);
export const BUSY_THRESHOLD_DBM = -70;
export const RSSI_MIN = -100; // chart bottom
export const RSSI_MAX = -30; // chart top

const COLOR_DATA = "#4ea1ff";
const COLOR_ADV = "#ff9d4e";

const channelLabels = Array.from({ length: NUM_CHANNELS }, (_, i) =>
  ADV_CHANNELS.has(i) ? `${i} (ADV)` : `${i}`
);

// ---------------------------------------------------------------- spectrum

export function createSpectrumChart(canvas) {
  const ctx = canvas.getContext("2d");
  return new Chart(ctx, {
    type: "bar",
    data: {
      labels: channelLabels,
      datasets: [
        {
          label: "RSSI (dBm)",
          data: new Array(NUM_CHANNELS).fill(RSSI_MIN),
          backgroundColor: channelLabels.map((_, i) =>
            ADV_CHANNELS.has(i) ? COLOR_ADV : COLOR_DATA
          ),
          borderWidth: 0,
        },
      ],
    },
    options: {
      responsive: true,
      maintainAspectRatio: false,
      animation: false,
      plugins: {
        legend: { display: false },
        tooltip: {
          callbacks: {
            title: (items) => `Channel ${items[0].label}`,
            label: (item) => `${item.raw} dBm`,
          },
        },
        // Horizontal "busy" threshold line, drawn via a small inline plugin.
        annotationLine: { value: BUSY_THRESHOLD_DBM },
      },
      scales: {
        x: {
          title: { display: true, text: "BLE channel index" },
          ticks: { color: "#8b97a7", autoSkip: true, maxRotation: 0 },
          grid: { display: false },
        },
        y: {
          min: RSSI_MIN,
          max: RSSI_MAX,
          title: { display: true, text: "RSSI (dBm)" },
          ticks: { color: "#8b97a7" },
          grid: { color: "#2a3340" },
        },
      },
    },
    plugins: [busyThresholdPlugin],
  });
}

// Draws a dashed horizontal line at the busy threshold over the bar chart.
const busyThresholdPlugin = {
  id: "busyThreshold",
  afterDraw(chart) {
    const { ctx, chartArea, scales } = chart;
    if (!scales.y) return;
    const y = scales.y.getPixelForValue(BUSY_THRESHOLD_DBM);
    if (y < chartArea.top || y > chartArea.bottom) return;
    ctx.save();
    ctx.beginPath();
    ctx.setLineDash([6, 4]);
    ctx.lineWidth = 1.5;
    ctx.strokeStyle = "#f0506e";
    ctx.moveTo(chartArea.left, y);
    ctx.lineTo(chartArea.right, y);
    ctx.stroke();
    ctx.setLineDash([]);
    ctx.fillStyle = "#f0506e";
    ctx.font = "11px system-ui, sans-serif";
    ctx.fillText(`busy ${BUSY_THRESHOLD_DBM} dBm`, chartArea.left + 6, y - 4);
    ctx.restore();
  },
};

export function updateSpectrumChart(chart, rssiArray) {
  // Clamp into the visible range so out-of-band values still render a bar.
  chart.data.datasets[0].data = rssiArray.map((v) =>
    Math.max(RSSI_MIN, Math.min(RSSI_MAX, v))
  );
  chart.update("none");
}

// ----------------------------------------------------------------- heatmap

// Map an RSSI value to a cool→warm colour (quiet=blue, busy=red).
function rssiToColor(rssi) {
  const t = Math.max(0, Math.min(1, (rssi - RSSI_MIN) / (RSSI_MAX - RSSI_MIN)));
  // Interpolate hue from 220 (blue) down to 0 (red).
  const hue = 220 * (1 - t);
  return `hsl(${hue}, 80%, 50%)`;
}

export function createHeatmapChart(canvas) {
  const ctx = canvas.getContext("2d");
  return new Chart(ctx, {
    type: "matrix",
    data: {
      datasets: [
        {
          label: "RSSI heatmap",
          data: [], // filled by updateHeatmapChart
          width: (c) => {
            const area = c.chart.chartArea;
            if (!area) return 2;
            const cols = c.chart.$heatmapCols || 1;
            return area.width / cols;
          },
          height: (c) => {
            const area = c.chart.chartArea;
            if (!area) return 2;
            return area.height / NUM_CHANNELS;
          },
          backgroundColor: (c) => rssiToColor(c.raw.v),
          borderWidth: 0,
        },
      ],
    },
    options: {
      responsive: true,
      maintainAspectRatio: false,
      animation: false,
      plugins: {
        legend: { display: false },
        tooltip: {
          callbacks: {
            title: () => "",
            label: (item) => {
              const r = item.raw;
              return `ch ${r.y} · ${r.v} dBm`;
            },
          },
        },
      },
      scales: {
        x: {
          type: "linear",
          title: { display: true, text: "time (older ← → newer)" },
          ticks: { display: false },
          grid: { display: false },
        },
        y: {
          type: "linear",
          min: -0.5,
          max: NUM_CHANNELS - 0.5,
          reverse: true,
          title: { display: true, text: "BLE channel" },
          ticks: {
            color: "#8b97a7",
            stepSize: 5,
            callback: (v) => (Number.isInteger(v) ? v : ""),
          },
          grid: { display: false },
        },
      },
    },
  });
}

export function updateHeatmapChart(chart, history) {
  const cols = history.length;
  chart.$heatmapCols = cols;

  // x axis spans the column count; each sweep is one column.
  chart.options.scales.x.min = -0.5;
  chart.options.scales.x.max = Math.max(0.5, cols - 0.5);

  const data = [];
  for (let col = 0; col < cols; col++) {
    const sweep = history[col];
    for (let ch = 0; ch < NUM_CHANNELS; ch++) {
      data.push({ x: col, y: ch, v: sweep.rssi[ch] });
    }
  }
  chart.data.datasets[0].data = data;
  chart.update("none");
}
