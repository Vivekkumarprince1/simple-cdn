'use strict';

const REGIONS = {
  north: { city: 'Delhi', label: 'Delhi edge', coordinates: [28.6139, 77.2090] },
  'west-central': { city: 'Mumbai', label: 'Mumbai edge', coordinates: [19.0760, 72.8777] },
  'south-east': { city: 'Bengaluru', label: 'Bengaluru edge', coordinates: [12.9716, 77.5946] }
};

const CITIES = {
  Delhi: [28.6139, 77.2090], Mumbai: [19.0760, 72.8777], Bengaluru: [12.9716, 77.5946],
  Chennai: [13.0827, 80.2707], Hyderabad: [17.3850, 78.4867]
};

const elements = {
  systemState: document.querySelector('#system-state'), systemLabel: document.querySelector('#system-state-label'),
  refresh: document.querySelector('#refresh-button'), autoRefresh: document.querySelector('#auto-refresh'),
  updated: document.querySelector('#last-updated'), alert: document.querySelector('#global-alert'),
  alertTitle: document.querySelector('#alert-title'), alertDetail: document.querySelector('#alert-detail'),
  alertRetry: document.querySelector('#alert-retry'), form: document.querySelector('#routing-form'),
  city: document.querySelector('#city'), routeButton: document.querySelector('#route-button'),
  locationButton: document.querySelector('#location-button'), routeResult: document.querySelector('#route-result'),
  routeBadge: document.querySelector('#route-badge'), decisionSource: document.querySelector('#decision-source'),
  selectedEdge: document.querySelector('#selected-edge'), requestedRegion: document.querySelector('#requested-region'),
  distance: document.querySelector('#route-distance'), pathState: document.querySelector('#path-state'),
  routeStatus: document.querySelector('#route-status'), fleetSummary: document.querySelector('#fleet-summary'),
  activity: document.querySelector('#activity-log'), clearActivity: document.querySelector('#clear-activity')
};

let currentPoint = null;
let intervalId = null;
let lastReadiness = null;

function formatNumber(value) {
  return Number(value || 0).toLocaleString('en-IN');
}

function distanceKm(origin, destination) {
  const radians = value => value * Math.PI / 180;
  const latitude = radians(destination[0] - origin[0]);
  const longitude = radians(destination[1] - origin[1]);
  const calculation = Math.sin(latitude / 2) ** 2 +
    Math.cos(radians(origin[0])) * Math.cos(radians(destination[0])) * Math.sin(longitude / 2) ** 2;
  return Math.round(6371 * 2 * Math.asin(Math.sqrt(calculation)));
}

function nearestRegion(point) {
  return Object.keys(REGIONS).reduce((nearest, region) =>
    distanceKm(point, REGIONS[region].coordinates) < distanceKm(point, REGIONS[nearest].coordinates) ? region : nearest
  );
}

function setSystemState(state, label) {
  elements.systemState.dataset.state = state;
  elements.systemLabel.textContent = label;
}

function showAlert(title, detail) {
  elements.alertTitle.textContent = title;
  elements.alertDetail.textContent = detail;
  elements.alert.hidden = false;
}

function hideAlert() { elements.alert.hidden = true; }

function updateFleet(edges) {
  const entries = Object.entries(REGIONS);
  let healthyCount = 0;
  for (const [region] of entries) {
    const healthy = edges?.[region] === true;
    if (healthy) healthyCount++;
    const state = healthy ? 'healthy' : 'failed';
    const card = document.querySelector(`#card-${region}`);
    const node = document.querySelector(`#node-${region}`);
    card.dataset.state = state;
    card.querySelector('.edge-status').textContent = healthy ? 'HEALTHY' : 'CIRCUIT OPEN';
    node.dataset.state = state;
    node.setAttribute('aria-label', `${REGIONS[region].city} ${region} edge ${healthy ? 'healthy' : 'circuit open'}`);
  }
  elements.fleetSummary.textContent = `${healthyCount}/${entries.length} EDGES HEALTHY`;
  const system = healthyCount === entries.length ? ['healthy', 'ALL SYSTEMS NOMINAL'] : healthyCount > 0 ? ['degraded', 'DEGRADED'] : ['error', 'ROUTING UNAVAILABLE'];
  setSystemState(...system);
}

function parseMetrics(text) {
  const values = {};
  for (const line of text.split('\n')) {
    if (!line || line.startsWith('#') || line.includes('{')) continue;
    const [name, value] = line.trim().split(/\s+/);
    values[name] = Number(value);
  }
  return values;
}

function updateMetrics(metrics) {
  const fields = [
    ['requests', metrics.cdn_requests_total], ['errors', metrics.cdn_errors_total],
    ['failovers', metrics.cdn_failovers_total], ['active', metrics.cdn_active_connections]
  ];
  for (const [name, value] of fields) {
    const target = document.querySelector(`#metric-${name}`);
    target.textContent = formatNumber(value);
    target.closest('.metric').classList.add('has-data');
  }
  const ratio = metrics.cdn_requests_total ? (metrics.cdn_errors_total / metrics.cdn_requests_total * 100).toFixed(2) : '0.00';
  document.querySelector('#error-ratio').textContent = `${ratio}% cumulative 4xx/5xx ratio`;
}

async function refreshOperations({ silent = false } = {}) {
  if (!silent) elements.refresh.classList.add('is-loading');
  try {
    const [readinessResponse, metricsResponse] = await Promise.all([
      fetch('/ready', { cache: 'no-store' }), fetch('/metrics', { cache: 'no-store' })
    ]);
    if (!readinessResponse.ok && readinessResponse.status !== 503) throw new Error(`readiness returned ${readinessResponse.status}`);
    if (!metricsResponse.ok) throw new Error(`metrics returned ${metricsResponse.status}`);
    const readiness = await readinessResponse.json();
    const metrics = parseMetrics(await metricsResponse.text());
    lastReadiness = readiness;
    updateFleet(readiness.edges);
    updateMetrics(metrics);
    elements.updated.textContent = `UPDATED ${new Date().toLocaleTimeString([], { hour: '2-digit', minute: '2-digit', second: '2-digit' })}`;
    hideAlert();
  } catch (error) {
    setSystemState('error', 'TELEMETRY OFFLINE');
    showAlert('Operational data unavailable', `${error.message}. The router may be stopped or metrics may require a token.`);
  } finally {
    elements.refresh.classList.remove('is-loading');
  }
}

function setActiveRoute(selected, failedOver) {
  document.querySelectorAll('[data-route]').forEach(path => {
    path.classList.toggle('is-active', path.dataset.route === selected);
    path.classList.toggle('is-failover', path.dataset.route === selected && failedOver);
  });
  document.querySelectorAll('.edge-node').forEach(node => node.classList.remove('is-selected'));
  document.querySelector(`#node-${selected}`)?.classList.add('is-selected');
}

function addActivity(message, code, type = '') {
  elements.activity.querySelector('.empty-activity')?.remove();
  const item = document.createElement('li');
  const time = document.createElement('time');
  time.dateTime = new Date().toISOString();
  time.textContent = new Date().toLocaleTimeString([], { hour: '2-digit', minute: '2-digit', second: '2-digit' });
  const copy = document.createElement('p'); copy.textContent = message;
  const status = document.createElement('span'); status.className = `activity-code ${type}`; status.textContent = code;
  item.append(time, copy, status);
  elements.activity.prepend(item);
  while (elements.activity.children.length > 8) elements.activity.lastElementChild.remove();
}

async function routeRequest(requestedRegion, point, sourceLabel) {
  elements.routeButton.disabled = true;
  elements.locationButton.disabled = true;
  elements.routeButton.querySelector('span').textContent = 'Routing…';
  elements.routeStatus.textContent = 'Resolving edge circuit and failover state…';
  elements.routeResult.dataset.state = 'loading';
  try {
    const response = await fetch(`/__cdn/route?region=${encodeURIComponent(requestedRegion)}`, { cache: 'no-store' });
    if (!response.ok) throw new Error(`router returned ${response.status}`);
    const result = await response.json();
    if (!result.available || !REGIONS[result.selected_region]) throw new Error('no healthy edge is available');
    const selected = result.selected_region;
    const failedOver = selected !== requestedRegion;
    elements.routeResult.dataset.state = failedOver ? 'failover' : 'healthy';
    elements.routeBadge.textContent = failedOver ? 'FAILOVER' : 'ROUTE HEALTHY';
    elements.decisionSource.textContent = sourceLabel.toUpperCase();
    elements.selectedEdge.textContent = REGIONS[selected].label;
    elements.requestedRegion.textContent = requestedRegion;
    elements.distance.textContent = point ? `${distanceKm(point, REGIONS[selected].coordinates).toLocaleString('en-IN')} km` : '—';
    elements.pathState.textContent = failedOver ? `${requestedRegion} bypassed` : 'Preferred edge';
    elements.routeStatus.textContent = failedOver ? `The ${requestedRegion} circuit is open. Traffic moved to ${selected}.` : `${REGIONS[selected].city} accepted the regional route.`;
    setActiveRoute(selected, failedOver);
    addActivity(`${sourceLabel} routed to ${REGIONS[selected].city}${failedOver ? ` after ${requestedRegion} failed` : ''}.`, failedOver ? 'FAILOVER' : '200 ROUTED', failedOver ? 'is-failover' : '');
    await refreshOperations({ silent: true });
  } catch (error) {
    elements.routeResult.dataset.state = 'error';
    elements.routeBadge.textContent = 'ROUTE FAILED';
    elements.selectedEdge.textContent = 'No edge available';
    elements.pathState.textContent = 'Unavailable';
    elements.routeStatus.textContent = `Unable to complete route: ${error.message}.`;
    addActivity(`Routing failed: ${error.message}.`, 'ERROR', 'is-error');
  } finally {
    elements.routeButton.disabled = false;
    elements.locationButton.disabled = false;
    elements.routeButton.querySelector('span').textContent = 'Route request';
  }
}

function routeSelectedCity(event) {
  event?.preventDefault();
  currentPoint = null;
  const point = CITIES[elements.city.value];
  routeRequest(nearestRegion(point), point, elements.city.value);
}

function useCurrentLocation() {
  if (!navigator.geolocation) {
    elements.routeStatus.textContent = 'This browser does not support geolocation. Choose a city instead.';
    addActivity('Browser geolocation is unavailable.', 'UNSUPPORTED', 'is-error');
    return;
  }
  elements.locationButton.disabled = true;
  elements.routeStatus.textContent = 'Waiting for location permission…';
  navigator.geolocation.getCurrentPosition(position => {
    currentPoint = [position.coords.latitude, position.coords.longitude];
    routeRequest(nearestRegion(currentPoint), currentPoint, 'Current location');
  }, error => {
    elements.locationButton.disabled = false;
    const denied = error.code === error.PERMISSION_DENIED;
    elements.routeStatus.textContent = denied ? 'Location permission was not granted. Choose a city or update site permissions.' : 'Location could not be resolved. Choose a city or try again.';
    addActivity(denied ? 'Location permission denied.' : 'Location lookup failed.', denied ? 'DENIED' : 'ERROR', 'is-error');
  }, { enableHighAccuracy: false, timeout: 10000, maximumAge: 300000 });
}

function configureAutoRefresh() {
  clearInterval(intervalId);
  if (elements.autoRefresh.checked) intervalId = setInterval(() => {
    if (document.visibilityState === 'visible') refreshOperations({ silent: true });
  }, 5000);
}

elements.form.addEventListener('submit', routeSelectedCity);
elements.locationButton.addEventListener('click', useCurrentLocation);
elements.refresh.addEventListener('click', () => refreshOperations());
elements.alertRetry.addEventListener('click', () => refreshOperations());
elements.autoRefresh.addEventListener('change', configureAutoRefresh);
elements.clearActivity.addEventListener('click', () => {
  elements.activity.replaceChildren();
  const empty = document.createElement('li'); empty.className = 'empty-activity'; empty.innerHTML = '<span>—</span><p>No routing actions in this browser session.</p>';
  elements.activity.append(empty);
});
document.querySelectorAll('[data-route-region]').forEach(button => button.addEventListener('click', () => {
  const region = button.dataset.routeRegion;
  routeRequest(region, REGIONS[region].coordinates, `${REGIONS[region].city} test`);
}));

configureAutoRefresh();
refreshOperations();
