// SPDX-License-Identifier: GPL-3.0-or-later
import {createPlayer} from '@videojs/html';
import {liveVideoFeatures, videoFeatures, playbackFeature} from '@videojs/core/dom';
import {pcmVolumeFeature, intentPlaybackFeature} from './videojs-features.js';
import {installPreview} from './recording-preview.js';

const features = liveVideoFeatures.map(feature => {
  if (feature.name === 'volume') return pcmVolumeFeature(window.pbsAudio);
  if (feature.name === 'playback') return intentPlaybackFeature(playbackFeature, window.pbsPlayback);
  // Native PiP/AirPlay would export only H.264, without the separate PCM.
  if (feature.name === 'pip' || feature.name === 'remotePlayback')
    return {...feature, attach() {}}; // Keep their UI state explicitly unavailable.
  return feature;
});
const {PlayerElement} = createPlayer({features});
class PbsVideoPlayer extends PlayerElement {
  static tagName = 'pbs-video-player';
}
customElements.define(PbsVideoPlayer.tagName, PbsVideoPlayer);
const {PlayerElement: PreviewElement}=createPlayer({features:videoFeatures.map(feature=>
  ['volume','pip','remotePlayback'].includes(feature.name)?{...feature,attach(){}}:feature)});
class PbsPreviewPlayer extends PreviewElement { static tagName='pbs-preview-player'; }
customElements.define(PbsPreviewPlayer.tagName,PbsPreviewPlayer);
// Upgrade the provider before its skin's consumers request the player context.
await import('@videojs/html/live-video/skin');
await import('@videojs/html/video/skin');
installPreview(window.pbsMux,window.pbsPlayback);
