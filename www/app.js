const $ = (s) => document.querySelector(s);

const clockEl = $("#clock");
const hourEl = $("#hour");
const minuteEl = $("#minute");
const ampmEl = $("#ampm");
const setBtn = $("#setBtn");
const enSwitch = $("#en");
const statusEl = $("#status");
const sunriseMin = $("#sunriseMin");

let alarmTimeMs = null;

const pad2 = (n) => String(n).padStart(2, "0");

function fmtClock(dt) 
{
  let h = dt.getHours();
  const ap = h >= 12 ? "PM" : "AM";
  h = h % 12 || 12;
  return `${pad2(h)}:${pad2(dt.getMinutes())}:${pad2(dt.getSeconds())} ${ap}`;
}

function toAbsoluteTimeMs(h12, m, ap) 
{
  const now = new Date();
  let h24 = h12 % 12;
  if (ap === "PM") h24 += 12;
  const t = new Date(now);
  t.setHours(h24, m, 0, 0);
  if (t.getTime() <= now.getTime()) t.setDate(t.getDate() + 1);
  return t.getTime();
}

async function api(path, options = {}) 
{
  const res = await fetch(path, {
    method: "GET",
    cache: "no-store",
    ...options,
    headers: { "Content-Type": "application/json", ...(options.headers || {}) }
  });
  if (!res.ok) throw new Error(`${res.status} ${res.statusText}`);
  const ct = res.headers.get("content-type") || "";
  return ct.includes("application/json") ? res.json() : res.text();
}

(function initDropdowns() {
  for (let h = 1; h <= 12; h++) hourEl.insertAdjacentHTML("beforeend", `<option value="${h}">${h}</option>`);
  for (let m = 0; m < 60; m++) minuteEl.insertAdjacentHTML("beforeend", `<option value="${m}">${pad2(m)}</option>`);
  const now = new Date();
  let h = now.getHours();
  const ap = h >= 12 ? "PM" : "AM";
  h = h % 12 || 12;
  let nextMin = now.getMinutes() + 1;
  if (nextMin >= 60) nextMin = 0;
  hourEl.value = String(h);
  minuteEl.value = String(nextMin);
  ampmEl.value = ap;
})();

setInterval(() => { clockEl.textContent = fmtClock(new Date()); }, 250);

setBtn.addEventListener('click', async () => {
    // Get the current date and time
    const now = new Date();

    // Get the selected time from the input, which is in "HH:mm" format
    const hour = parseInt(hourEl.value, 10);
    const minute = parseInt(minuteEl.value, 10);
    const sunrise = parseInt(sunriseMin.value, 10)
    const am_pm = ampmEl.value;
    if (isNaN(hour) || isNaN(minute)) {
        statusEl.textContent = 'Please select a valid time.';
        return;
    }
    let hour_24 = hour % 12;
    if (am_pm === "PM") hour_24 += 12;
    
    // The URL for your ESP32's PUT handler
    const url = '/api/v1/alarm';

    try {
        const response = await fetch(url, {
            method: 'PUT',
            headers: {
                'Content-Type': 'application/json'
            },
            body: JSON.stringify({ hours:hour_24, minutes:minute, sunrise: sunrise })
        });

        if (response.ok) 
        {
            statusEl.textContent = 'Alarm set successfully!';
        } 
        else 
        {
            const errorText = await response.text();
            statusEl.textContent = `Error: ${errorText}`;
            console.error('Server error:', response.status, errorText);
        }
    } catch (error) 
    {
        statusEl.textContent = `Network error: ${error.message}`;
        console.error('Fetch error:', error);
    }
});

en.addEventListener('change', async () => 
{
    // The URL for your ESP32's PUT handler
    const url = '/api/v1/enable';

    try {
        const response = await fetch(url, {
            method: 'PUT',
            headers: {
                'Content-Type': 'application/json'
            },
            body: JSON.stringify({ enabled: en.checked })
        });

        if (response.ok) 
        {
            statusEl.textContent = 'Alarm enabled successfully!';
        } 
        else 
        {
            const errorText = await response.text();
            statusEl.textContent = `Error: ${errorText}`;
            console.error('Server error:', response.status, errorText);
        }
    } catch (error) 
    {
        statusEl.textContent = `Network error: ${error.message}`;
        console.error('Fetch error:', error);
    }
});

async function loadConfig() 
{
    try 
    {
        const url = '/api/v1/alarm';
        const resp = await fetch(url, {cache:'no-store'});
        if (!resp.ok) throw new Error(resp.statusText);
        const s = await resp.json();

        // populate fields
        hourEl.value = s.hours % 12;
        minuteEl.value = s.minutes;
        ampmEl.value = (s.hours / 12) > 0 ? "PM" : "AM"
        enSwitch.checked = s.enabled;
        sunriseMin.value = s.sunrise;
    } 
    catch (err) 
    {
        statusEl.textContent = `Failed to load config. error: ${error.message}`;
        console.error('Failed to load config. error:', error);
    }
}

window.addEventListener('DOMContentLoaded', loadConfig);