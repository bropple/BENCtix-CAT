// SPDX-License-Identifier: MIT
// s22-browser: YouTube without its ads (D054). Injected into youtube.com
// pages at document start, in the page's own world, while ad blocking is on.
//
// YouTube serves its ads from its own servers, inside the same data as the
// video, so request blocking cannot reach them. As uBlock Origin's YouTube
// rules do, this takes the ad entries out of the player's data before the
// page reads it: wherever JSON is parsed or a fetch answer is read, and the
// initial player data set by the page. Whatever still plays is skipped:
// an ad that shows is fast-forwarded, muted, and its Skip button pressed.
// YouTube changes this regularly; this file is where to follow it.
(function () {
	'use strict';
	const AD_KEYS = ['adPlacements', 'adSlots', 'playerAds', 'adBreakHeartbeatParams'];

	function prune(o, depth) {
		if (!o || typeof o !== 'object' || depth > 6)
			return o;
		for (const k of AD_KEYS)
			if (Object.prototype.hasOwnProperty.call(o, k)) {
				try { delete o[k]; } catch (e) { /* frozen: leave it */ }
			}
		if (Array.isArray(o)) {
			for (const x of o)
				prune(x, depth + 1);
		} else {
			if (o.playerResponse)
				prune(o.playerResponse, depth + 1);
			if (o.response)
				prune(o.response, depth + 1);
		}
		return o;
	}

	const parse = JSON.parse;
	JSON.parse = function () {
		return prune(parse.apply(this, arguments), 0);
	};

	const json = Response.prototype.json;
	Response.prototype.json = function () {
		return json.call(this).then((o) => prune(o, 0));
	};

	for (const name of ['ytInitialPlayerResponse', 'ytPlayerResponse']) {
		let value;
		try {
			Object.defineProperty(window, name, {
				configurable: true,
				get() { return value; },
				set(v) { value = prune(v, 0); },
			});
		} catch (e) { /* already defined */ }
	}

	// What still gets through
	function skip() {
		const player = document.querySelector('.ad-showing, .ad-interrupting');
		if (!player)
			return;
		const video = player.querySelector('video');
		if (video && isFinite(video.duration) && video.duration > 0) {
			video.muted = true;
			video.currentTime = video.duration;
		}
		const button = document.querySelector(
			'.ytp-ad-skip-button, .ytp-ad-skip-button-modern, .ytp-skip-ad-button, ' +
			'.ytp-ad-skip-button-container button, .ytm-skip-ad-button');
		if (button)
			button.click();
	}
	setInterval(skip, 400);

	// Ad slots in the page itself (the mobile site's and the desktop's)
	const css = 'ytm-promoted-sparkles-web-renderer, ytm-promoted-video-renderer, ' +
		'ytm-companion-ad-renderer, ytm-companion-slot, ad-slot-renderer, ' +
		'ytd-ad-slot-renderer, ytd-in-feed-ad-layout-renderer, #player-ads, ' +
		'.ytp-ad-overlay-container, ytm-mealbar-promo-renderer ' +
		'{ display: none !important; }';
	function style() {
		const s = document.createElement('style');
		s.textContent = css;
		(document.head || document.documentElement).appendChild(s);
	}
	if (document.readyState === 'loading')
		document.addEventListener('DOMContentLoaded', style);
	else
		style();
})();
