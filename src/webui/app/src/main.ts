import { mount } from 'svelte';
import './styles/fonts.css';
import './styles/themes.css';
import './styles/base.css';
import App from './App.svelte';

const target = document.getElementById('app');
if (!target) throw new Error('no #app element');

export default mount(App, { target });
