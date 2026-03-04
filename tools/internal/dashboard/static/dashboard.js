// QPS line chart using Canvas 2D API.
// Data is fed by htmx polling /api/chart-data which injects a <script> calling updateChart().

(function() {
    "use strict";

    var canvas = document.getElementById("qps-chart");
    if (!canvas) return;

    var ctx = canvas.getContext("2d");
    var data = [];

    function resize() {
        var parent = canvas.parentElement;
        canvas.width = parent.clientWidth;
        canvas.height = parent.clientHeight;
    }

    function draw() {
        resize();
        var w = canvas.width;
        var h = canvas.height;
        var pad = { top: 10, right: 10, bottom: 25, left: 50 };
        var plotW = w - pad.left - pad.right;
        var plotH = h - pad.top - pad.bottom;

        ctx.clearRect(0, 0, w, h);

        if (data.length === 0) {
            ctx.fillStyle = "#999";
            ctx.font = "14px sans-serif";
            ctx.textAlign = "center";
            ctx.fillText("Waiting for data...", w / 2, h / 2);
            return;
        }

        // Determine Y scale.
        var maxQPS = 0;
        for (var i = 0; i < data.length; i++) {
            if (data[i].qps > maxQPS) maxQPS = data[i].qps;
        }
        if (maxQPS === 0) maxQPS = 1;
        maxQPS = Math.ceil(maxQPS * 1.2); // Add 20% headroom.

        // Draw grid lines.
        ctx.strokeStyle = "#e0e0e0";
        ctx.lineWidth = 1;
        var gridLines = 4;
        for (var g = 0; g <= gridLines; g++) {
            var gy = pad.top + plotH - (plotH * g / gridLines);
            ctx.beginPath();
            ctx.moveTo(pad.left, gy);
            ctx.lineTo(pad.left + plotW, gy);
            ctx.stroke();

            // Y-axis labels.
            ctx.fillStyle = "#999";
            ctx.font = "11px sans-serif";
            ctx.textAlign = "right";
            ctx.fillText((maxQPS * g / gridLines).toFixed(1), pad.left - 5, gy + 4);
        }

        // Draw QPS line.
        ctx.strokeStyle = "#1095c1";
        ctx.lineWidth = 2;
        ctx.beginPath();
        for (var j = 0; j < data.length; j++) {
            var x = pad.left + (plotW * j / (data.length - 1 || 1));
            var y = pad.top + plotH - (plotH * data[j].qps / maxQPS);
            if (j === 0) {
                ctx.moveTo(x, y);
            } else {
                ctx.lineTo(x, y);
            }
        }
        ctx.stroke();

        // Fill area under line.
        ctx.lineTo(pad.left + plotW, pad.top + plotH);
        ctx.lineTo(pad.left, pad.top + plotH);
        ctx.closePath();
        ctx.fillStyle = "rgba(16, 149, 193, 0.1)";
        ctx.fill();

        // X-axis label.
        ctx.fillStyle = "#999";
        ctx.font = "11px sans-serif";
        ctx.textAlign = "center";
        ctx.fillText("~ " + (data.length * 2) + "s ago", pad.left, h - 3);
        ctx.fillText("now", pad.left + plotW, h - 3);
    }

    // Called from the htmx-injected <script> in chart.html partial.
    window.updateChart = function(points) {
        data = points || [];
        draw();
    };

    // Redraw on resize.
    window.addEventListener("resize", draw);

    // Initial draw.
    draw();
})();
