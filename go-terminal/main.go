// Seawolf — a terminal submarine periscope game.
//
// You command a submarine. Ships cross the horizon in several lanes; line up
// your periscope, lead your target and fire torpedoes before time runs out.
// Floating mines block torpedoes, and destroyers drop depth charges that can
// knock your periscope out for a moment.
package main

import (
	"fmt"
	"math"
	"math/rand"
	"os"
	"path/filepath"
	"strconv"
	"strings"
	"time"

	"github.com/gdamore/tcell/v2"
)

const (
	minW, minH     = 60, 22
	roundTime      = 90.0
	bonusScore     = 3000
	bonusTime      = 30.0
	maxTorpedoes   = 4
	reloadTime     = 1.1
	torpedoSpeed   = 16.0 // rows per second
	chargeSpeed    = 7.0
	stunTime       = 2.0
	stunTimePenalt = 5.0
	frameRate      = 30
)

// ---------------------------------------------------------------- ship types

type shipKind struct {
	name   string
	sprite [2]string // right-facing; row 1 is the waterline (hit row)
	speed  float64   // cells per second
	points int
	color  tcell.Color
	drops  bool // drops depth charges
}

var shipKinds = []shipKind{
	{"tanker", [2]string{"         _|_  ", `\____________/`}, 4.5, 100, tcell.ColorSilver, false},
	{"freighter", [2]string{"  _|__|__|_ ", `\_o_o_o_o__/`}, 6, 150, tcell.ColorYellow, false},
	{"destroyer", [2]string{"    __/^\\_  ", `\_=_=_=_=__>`}, 9, 250, tcell.ColorLightSteelBlue, true},
	{"patrol boat", [2]string{"  _/|", `\___>`}, 15, 500, tcell.ColorOrange, false},
}

// mirror flips a sprite row so a ship can sail right-to-left.
func mirror(s string) string {
	swap := map[rune]rune{'/': '\\', '\\': '/', '<': '>', '>': '<', '(': ')', ')': '('}
	r := []rune(s)
	for i, j := 0, len(r)-1; i < j; i, j = i+1, j-1 {
		r[i], r[j] = r[j], r[i]
	}
	for i, c := range r {
		if m, ok := swap[c]; ok {
			r[i] = m
		}
	}
	return string(r)
}

// --------------------------------------------------------------- game objects

type ship struct {
	kind    *shipKind
	lane    int
	x       float64
	dir     float64 // +1 right, -1 left
	sinking float64 // >0 while sinking (seconds left)
	dropCD  float64
}

func (s *ship) width() int { return len([]rune(s.kind.sprite[1])) }

func (s *ship) row(i int) string {
	if s.dir < 0 {
		return mirror(s.kind.sprite[i])
	}
	return s.kind.sprite[i]
}

type torpedo struct {
	x    int
	y    float64
	dead bool
}

type mine struct {
	x, y float64
	vx   float64
	dead bool
}

type charge struct {
	x    int
	y    float64
	dead bool
}

type particle struct {
	x, y, vx, vy, life float64
	ch                 rune
	color              tcell.Color
}

type popup struct {
	x, y int
	text string
	life float64
}

type state int

const (
	stTitle state = iota
	stPlaying
	stPaused
	stOver
)

type game struct {
	scr   tcell.Screen
	w, h  int
	rng   *rand.Rand
	state state

	score, hi  int
	timeLeft   float64
	bonusGiven bool
	shotsFired int
	hits       int
	combo      int
	newHi      bool

	px         int // periscope x
	tubes      int
	reload     float64
	stun       float64
	shake      float64
	spawnTimer float64
	mineTimer  float64
	clock      float64

	lanes     []int
	ships     []*ship
	torps     []*torpedo
	mines     []*mine
	charges   []*charge
	particles []*particle
	popups    []*popup
}

func hiPath() string {
	home, err := os.UserHomeDir()
	if err != nil {
		return ".seawolf_hi"
	}
	return filepath.Join(home, ".seawolf_hi")
}

func loadHi() int {
	b, err := os.ReadFile(hiPath())
	if err != nil {
		return 0
	}
	n, _ := strconv.Atoi(strings.TrimSpace(string(b)))
	return n
}

func saveHi(n int) { _ = os.WriteFile(hiPath(), []byte(strconv.Itoa(n)), 0o644) }

func (g *game) horizon() int   { return 3 }
func (g *game) playerRow() int { return g.h - 2 }
func (g *game) launchRow() int { return g.h - 4 }

func (g *game) layout() {
	g.w, g.h = g.scr.Size()
	top := g.horizon() + 3
	bottom := g.h - 7
	n := 4
	g.lanes = g.lanes[:0]
	for i := 0; i < n; i++ {
		g.lanes = append(g.lanes, top+(bottom-top)*i/(n-1))
	}
	if g.px <= 0 || g.px >= g.w-1 {
		g.px = g.w / 2
	}
}

func (g *game) reset() {
	g.score, g.hits, g.shotsFired, g.combo = 0, 0, 0, 0
	g.timeLeft = roundTime
	g.bonusGiven, g.newHi = false, false
	g.tubes, g.reload, g.stun, g.shake = maxTorpedoes, 0, 0, 0
	g.spawnTimer, g.mineTimer = 0.5, 3
	g.ships, g.torps, g.mines, g.charges = nil, nil, nil, nil
	g.particles, g.popups = nil, nil
	g.px = g.w / 2
}

// ------------------------------------------------------------------- update

func (g *game) fire() {
	if g.stun > 0 || g.tubes == 0 {
		return
	}
	for _, t := range g.torps { // one torpedo per column at a time
		if t.x == g.px && t.y > float64(g.launchRow()-3) {
			return
		}
	}
	g.tubes--
	g.shotsFired++
	g.torps = append(g.torps, &torpedo{x: g.px, y: float64(g.launchRow())})
}

func (g *game) laneFree(lane int, dir float64) bool {
	for _, s := range g.ships {
		if s.lane != lane {
			continue
		}
		if s.dir != dir {
			return false // keep lanes one-way to avoid ships passing through each other
		}
		if dir > 0 && s.x < 6 {
			return false
		}
		if dir < 0 && s.x+float64(s.width()) > float64(g.w-6) {
			return false
		}
	}
	return true
}

func (g *game) spawnShip() {
	lane := g.rng.Intn(len(g.lanes))
	dir := 1.0
	if g.rng.Intn(2) == 0 {
		dir = -1
	}
	if !g.laneFree(lane, dir) {
		return
	}
	// Fast boats are rarer; destroyers become more common as the clock runs down.
	weights := []int{3, 4, 3, 1}
	if g.timeLeft < 45 {
		weights = []int{2, 3, 4, 2}
	}
	total := 0
	for _, w := range weights {
		total += w
	}
	r := g.rng.Intn(total)
	k := 0
	for i, w := range weights {
		if r < w {
			k = i
			break
		}
		r -= w
	}
	s := &ship{kind: &shipKinds[k], lane: lane, dir: dir, dropCD: 1 + g.rng.Float64()*2}
	if dir > 0 {
		s.x = -float64(s.width())
	} else {
		s.x = float64(g.w)
	}
	g.ships = append(g.ships, s)
}

func (g *game) explode(x, y int, n int, colors []tcell.Color) {
	chars := []rune{'*', '#', '+', '.', '\'', '`', 'o'}
	for i := 0; i < n; i++ {
		a := g.rng.Float64() * math.Pi
		sp := 4 + g.rng.Float64()*10
		g.particles = append(g.particles, &particle{
			x: float64(x), y: float64(y),
			vx: math.Cos(a) * sp * 1.8, vy: -math.Sin(a) * sp,
			life:  0.4 + g.rng.Float64()*0.6,
			ch:    chars[g.rng.Intn(len(chars))],
			color: colors[g.rng.Intn(len(colors))],
		})
	}
}

func (g *game) addScore(pts int) {
	before := g.score
	g.score += pts
	if !g.bonusGiven && before < bonusScore && g.score >= bonusScore {
		g.bonusGiven = true
		g.timeLeft += bonusTime
		g.popups = append(g.popups, &popup{x: g.w/2 - 8, y: g.h / 2, text: "EXTENDED TIME +30", life: 2.5})
	}
	if g.score > g.hi {
		g.hi = g.score
		g.newHi = true
	}
}

func (g *game) update(dt float64) {
	g.clock += dt
	if g.state != stPlaying {
		g.updateEffects(dt)
		return
	}

	g.timeLeft -= dt
	if g.timeLeft <= 0 {
		g.timeLeft = 0
		g.state = stOver
		if g.newHi {
			saveHi(g.hi)
		}
		return
	}

	if g.stun > 0 {
		g.stun -= dt
	}
	if g.shake > 0 {
		g.shake -= dt
	}

	// Reload tubes one at a time.
	if g.tubes < maxTorpedoes {
		g.reload += dt
		if g.reload >= reloadTime {
			g.reload = 0
			g.tubes++
		}
	} else {
		g.reload = 0
	}

	// Spawning.
	g.spawnTimer -= dt
	if g.spawnTimer <= 0 {
		g.spawnShip()
		g.spawnTimer = 0.8 + g.rng.Float64()*1.4
	}
	g.mineTimer -= dt
	if g.mineTimer <= 0 && len(g.mines) < 3 && len(g.lanes) > 1 {
		i := g.rng.Intn(len(g.lanes) - 1)
		y := float64(g.lanes[i]+g.lanes[i+1]) / 2
		vx := 2 + g.rng.Float64()*3
		x := -1.0
		if g.rng.Intn(2) == 0 {
			vx, x = -vx, float64(g.w)
		}
		g.mines = append(g.mines, &mine{x: x, y: y, vx: vx})
		g.mineTimer = 4 + g.rng.Float64()*5
	}

	// Ships.
	for _, s := range g.ships {
		if s.sinking > 0 {
			s.sinking -= dt
			if s.sinking <= 0 {
				s.sinking = -1 // fully sunk; removed in cleanup
			}
			continue
		}
		s.x += s.dir * s.kind.speed * dt
		if s.kind.drops && s.x > 2 && s.x+float64(s.width()) < float64(g.w-2) {
			s.dropCD -= dt
			if s.dropCD <= 0 {
				s.dropCD = 1.5 + g.rng.Float64()*2.5
				g.charges = append(g.charges, &charge{
					x: int(s.x) + s.width()/2,
					y: float64(g.lanes[s.lane] + 1),
				})
			}
		}
	}

	// Mines.
	for _, m := range g.mines {
		m.x += m.vx * dt
		if m.x < -2 || m.x > float64(g.w+2) {
			m.dead = true
		}
	}

	// Torpedoes: step row by row so fast torpedoes never skip a lane.
	for _, t := range g.torps {
		prev := t.y
		t.y -= torpedoSpeed * dt
		for row := int(math.Ceil(prev)) - 1; row >= int(math.Ceil(t.y)) && !t.dead; row-- {
			g.torpedoAt(t, row)
		}
		if !t.dead && t.y <= float64(g.horizon()) {
			t.dead = true
			g.combo = 0
			g.explode(t.x, g.horizon()+1, 4, []tcell.Color{tcell.ColorWhite, tcell.ColorAqua})
		}
	}

	// Depth charges.
	for _, c := range g.charges {
		c.y += chargeSpeed * dt
		if int(c.y) >= g.playerRow()-1 {
			c.dead = true
			g.explode(c.x, g.playerRow()-1, 10, []tcell.Color{tcell.ColorWhite, tcell.ColorAqua, tcell.ColorBlue})
			if abs(c.x-g.px) <= 4 && g.stun <= 0 {
				g.stun = stunTime
				g.shake = 0.6
				g.timeLeft = math.Max(0.1, g.timeLeft-stunTimePenalt)
				g.combo = 0
				g.popups = append(g.popups, &popup{x: g.px - 7, y: g.playerRow() - 3, text: "HIT! -5 SEC", life: 1.5})
			}
		}
	}

	g.updateEffects(dt)
	g.cleanup()
}

func (g *game) torpedoAt(t *torpedo, row int) {
	// Mines.
	for _, m := range g.mines {
		if !m.dead && int(math.Round(m.y)) == row && abs(int(math.Round(m.x))-t.x) <= 0 {
			m.dead, t.dead = true, true
			t.y = float64(row)
			g.combo = 0
			g.explode(t.x, row, 12, []tcell.Color{tcell.ColorRed, tcell.ColorYellow})
			return
		}
	}
	// Ships: waterline row only.
	for _, s := range g.ships {
		if s.sinking > 0 || g.lanes[s.lane] != row {
			continue
		}
		left := int(math.Floor(s.x))
		if t.x >= left && t.x < left+s.width() {
			t.dead = true
			t.y = float64(row)
			s.sinking = 1.0
			g.hits++
			g.combo++
			// Farther lanes are worth more; consecutive hits multiply.
			mult := 1.0 + 0.5*float64(len(g.lanes)-1-s.lane)
			pts := int(float64(s.kind.points)*mult) * min(g.combo, 5)
			g.addScore(pts)
			label := strconv.Itoa(pts)
			if g.combo > 1 {
				label += fmt.Sprintf(" x%d", min(g.combo, 5))
			}
			g.popups = append(g.popups, &popup{x: t.x - len(label)/2, y: row - 2, text: label, life: 1.2})
			g.explode(t.x, row, 25, []tcell.Color{tcell.ColorRed, tcell.ColorOrange, tcell.ColorYellow, tcell.ColorWhite})
			return
		}
	}
}

func (g *game) updateEffects(dt float64) {
	for _, p := range g.particles {
		p.x += p.vx * dt
		p.y += p.vy * dt
		p.vy += 18 * dt
		p.life -= dt
	}
	for _, p := range g.popups {
		p.life -= dt
	}
	g.particles = filter(g.particles, func(p *particle) bool { return p.life > 0 })
	g.popups = filter(g.popups, func(p *popup) bool { return p.life > 0 })
}

func (g *game) cleanup() {
	g.ships = filter(g.ships, func(s *ship) bool {
		if s.sinking < 0 {
			return false
		}
		if s.sinking == 0 {
			if s.dir > 0 && s.x > float64(g.w+1) {
				return false
			}
			if s.dir < 0 && s.x+float64(s.width()) < -1 {
				return false
			}
		}
		return true
	})
	g.torps = filter(g.torps, func(t *torpedo) bool { return !t.dead })
	g.mines = filter(g.mines, func(m *mine) bool { return !m.dead })
	g.charges = filter(g.charges, func(c *charge) bool { return !c.dead })
}

// ------------------------------------------------------------------- render

func (g *game) put(x, y int, r rune, st tcell.Style) {
	if x >= 0 && x < g.w && y >= 0 && y < g.h {
		g.scr.SetContent(x, y, r, nil, st)
	}
}

func (g *game) text(x, y int, s string, st tcell.Style) {
	for i, r := range []rune(s) {
		g.put(x+i, y, r, st)
	}
}

func (g *game) center(y int, s string, st tcell.Style) {
	g.text((g.w-len([]rune(s)))/2, y, s, st)
}

func (g *game) draw() {
	g.scr.Clear()
	if g.w < minW || g.h < minH {
		g.center(g.h/2, fmt.Sprintf("Terminal too small (%dx%d) — need %dx%d", g.w, g.h, minW, minH),
			tcell.StyleDefault.Foreground(tcell.ColorRed))
		g.scr.Show()
		return
	}

	sky := tcell.StyleDefault.Background(tcell.NewRGBColor(10, 14, 40))
	sea := tcell.StyleDefault.Background(tcell.NewRGBColor(0, 30, 70))
	if g.stun > 0 && int(g.clock*12)%2 == 0 {
		sea = tcell.StyleDefault.Background(tcell.NewRGBColor(40, 40, 60))
	}

	// Background.
	for y := 0; y < g.h; y++ {
		st := sea
		if y <= g.horizon() {
			st = sky
		}
		for x := 0; x < g.w; x++ {
			g.put(x, y, ' ', st)
		}
	}
	// Stars.
	for i := 0; i < g.w/6; i++ {
		x := (i*37 + 11) % g.w
		y := 1 + (i*13)%(g.horizon()-1)
		if (i+int(g.clock*2))%9 != 0 {
			g.put(x, y, '.', sky.Foreground(tcell.ColorGray))
		}
	}
	// Horizon and waves.
	for x := 0; x < g.w; x++ {
		g.put(x, g.horizon(), '~', sky.Foreground(tcell.ColorSteelBlue))
	}
	waveSt := sea.Foreground(tcell.NewRGBColor(40, 90, 150))
	for y := g.horizon() + 1; y < g.h-3; y++ {
		for x := 0; x < g.w; x++ {
			v := math.Sin(float64(x)*0.35+float64(y)*1.7+g.clock*1.5) +
				math.Sin(float64(x)*0.11-g.clock*0.7+float64(y))
			if v > 1.65 {
				g.put(x, y, '~', waveSt)
			}
		}
	}

	// Ships.
	for _, s := range g.ships {
		col := s.kind.color
		sink := 0
		if s.sinking > 0 {
			if int(g.clock*15)%2 == 0 {
				col = tcell.ColorRed
			} else {
				col = tcell.ColorOrange
			}
			sink = int((1 - s.sinking) * 2.5) // slide under the waterline
		}
		st := sea.Foreground(col)
		base := g.lanes[s.lane]
		left := int(math.Floor(s.x))
		for i := 0; i < 2; i++ {
			y := base - 1 + i + sink
			if y > base { // below waterline: hidden
				continue
			}
			for j, r := range []rune(s.row(i)) {
				if r != ' ' {
					g.put(left+j, y, r, st)
				}
			}
		}
	}

	// Mines.
	for _, m := range g.mines {
		ch := '@'
		if int(g.clock*4)%2 == 0 {
			ch = '0'
		}
		g.put(int(math.Round(m.x)), int(math.Round(m.y)), ch, sea.Foreground(tcell.ColorRed).Bold(true))
	}

	// Torpedoes with wake.
	for _, t := range g.torps {
		y := int(math.Round(t.y))
		g.put(t.x, y, '^', sea.Foreground(tcell.ColorWhite).Bold(true))
		for k := 1; k <= 3; k++ {
			if y+k < g.launchRow()+1 {
				g.put(t.x, y+k, []rune{'|', ':', '.'}[k-1], sea.Foreground(tcell.NewRGBColor(150, 200, 230)))
			}
		}
	}

	// Depth charges.
	for _, c := range g.charges {
		g.put(c.x, int(c.y), 'o', sea.Foreground(tcell.ColorDarkGray).Bold(true))
	}

	// Particles.
	for _, p := range g.particles {
		y := int(math.Round(p.y))
		st := sea
		if y <= g.horizon() {
			st = sky
		}
		g.put(int(math.Round(p.x)), y, p.ch, st.Foreground(p.color))
	}

	// Periscope.
	g.drawPeriscope(sea)

	// Popups.
	for _, p := range g.popups {
		g.text(p.x, p.y, p.text, sea.Foreground(tcell.ColorYellow).Bold(true))
	}

	g.drawHUD()

	switch g.state {
	case stTitle:
		g.drawTitle()
	case stPaused:
		g.center(g.h/2, "  PAUSED — press P to resume  ", tcell.StyleDefault.Reverse(true))
	case stOver:
		g.drawGameOver()
	}
	g.scr.Show()
}

func (g *game) drawPeriscope(sea tcell.Style) {
	x := g.px
	if g.shake > 0 {
		x += g.rng.Intn(3) - 1
	}
	col := tcell.ColorLime
	if g.stun > 0 {
		col = tcell.ColorDarkGray
	} else if g.tubes == 0 {
		col = tcell.ColorOlive
	}
	st := sea.Foreground(col).Bold(true)
	y := g.playerRow()
	g.put(x, y-2, '+', st)
	g.put(x, y-1, '|', st)
	g.text(x-4, y, "[===O===]", st)
	for i := 0; i < g.w; i++ {
		g.put(i, g.h-1, '▀', tcell.StyleDefault.Foreground(tcell.NewRGBColor(20, 20, 30)).Background(tcell.ColorBlack))
	}
}

func (g *game) drawHUD() {
	st := tcell.StyleDefault.Background(tcell.NewRGBColor(10, 14, 40)).Foreground(tcell.ColorWhite)
	hl := st.Foreground(tcell.ColorYellow).Bold(true)
	g.text(1, 0, "SCORE", st)
	g.text(7, 0, fmt.Sprintf("%6d", g.score), hl)
	g.text(16, 0, "HI", st)
	g.text(19, 0, fmt.Sprintf("%6d", g.hi), hl)

	t := fmt.Sprintf("TIME %3d", int(math.Ceil(g.timeLeft)))
	tst := hl
	if g.timeLeft < 10 && int(g.clock*4)%2 == 0 {
		tst = st.Foreground(tcell.ColorRed).Bold(true)
	}
	g.text(g.w/2-4, 0, t, tst)

	var b strings.Builder
	for i := 0; i < maxTorpedoes; i++ {
		if i < g.tubes {
			b.WriteRune('▮')
		} else {
			b.WriteRune('▯')
		}
	}
	label := "TUBES "
	g.text(g.w-len(label)-maxTorpedoes-1, 0, label, st)
	g.text(g.w-maxTorpedoes-1, 0, b.String(), st.Foreground(tcell.ColorLime))
	if g.combo > 1 {
		g.text(g.w-len(label)-maxTorpedoes-10, 0, fmt.Sprintf("COMBO x%d", min(g.combo, 5)), st.Foreground(tcell.ColorOrange))
	}
}

func (g *game) box(lines []string, y0 int) {
	wd := 0
	for _, l := range lines {
		wd = max(wd, len([]rune(l)))
	}
	wd += 4
	x0 := (g.w - wd) / 2
	st := tcell.StyleDefault.Background(tcell.ColorBlack).Foreground(tcell.ColorWhite)
	for y := 0; y < len(lines)+2; y++ {
		for x := 0; x < wd; x++ {
			g.put(x0+x, y0+y, ' ', st)
		}
	}
	for i, l := range lines {
		s := st
		if i == 0 {
			s = st.Foreground(tcell.ColorAqua).Bold(true)
		}
		g.center(y0+1+i, l, s)
	}
}

func (g *game) drawTitle() {
	lines := []string{
		"S E A W O L F",
		"",
		"Sink as many ships as you can before time runs out.",
		"Torpedoes take time to travel — lead your targets!",
		"",
		"  tanker 100   freighter 150   destroyer 250   patrol boat 500  ",
		"Far lanes score more. Consecutive hits multiply (up to x5).",
		"Mines (@) block torpedoes. Destroyers drop depth charges (o):",
		"one landing on your periscope costs 5 seconds.",
		fmt.Sprintf("Reach %d points for +%d seconds.", bonusScore, int(bonusTime)),
		"",
		"←/→ or A/D  move      SPACE  fire      P  pause      Q  quit",
		"",
		"Press ENTER to dive",
	}
	g.box(lines, max(1, (g.h-len(lines)-2)/2))
}

func (g *game) drawGameOver() {
	acc := 0
	if g.shotsFired > 0 {
		acc = g.hits * 100 / g.shotsFired
	}
	lines := []string{
		"T I M E ' S   U P",
		"",
		fmt.Sprintf("Score: %d", g.score),
		fmt.Sprintf("Ships sunk: %d    Accuracy: %d%%", g.hits, acc),
	}
	if g.newHi {
		lines = append(lines, "", "*** NEW HIGH SCORE ***")
	}
	lines = append(lines, "", "ENTER to play again   Q to quit")
	g.box(lines, max(1, (g.h-len(lines)-2)/2))
}

// --------------------------------------------------------------------- input

func (g *game) key(ev *tcell.EventKey) bool {
	k, r := ev.Key(), ev.Rune()
	if k == tcell.KeyRune {
		r = []rune(strings.ToLower(string(r)))[0]
	}
	if k == tcell.KeyEscape || k == tcell.KeyCtrlC || (k == tcell.KeyRune && r == 'q') {
		if g.state == stPlaying || g.state == stPaused {
			if g.newHi {
				saveHi(g.hi)
			}
		}
		return false
	}
	switch g.state {
	case stTitle, stOver:
		if k == tcell.KeyEnter || (k == tcell.KeyRune && r == ' ' && g.state == stTitle) {
			g.reset()
			g.state = stPlaying
		}
	case stPaused:
		if k == tcell.KeyRune && r == 'p' {
			g.state = stPlaying
		}
	case stPlaying:
		step := 2
		if ev.Modifiers()&tcell.ModShift != 0 {
			step = 5
		}
		switch {
		case k == tcell.KeyLeft || (k == tcell.KeyRune && r == 'a'):
			g.px = max(4, g.px-step)
		case k == tcell.KeyRight || (k == tcell.KeyRune && r == 'd'):
			g.px = min(g.w-5, g.px+step)
		case k == tcell.KeyUp || (k == tcell.KeyRune && (r == ' ' || r == 'w')):
			g.fire()
		case k == tcell.KeyRune && r == 'p':
			g.state = stPaused
		}
	}
	return true
}

// ---------------------------------------------------------------------- main

func main() {
	scr, err := tcell.NewScreen()
	if err != nil {
		fmt.Fprintln(os.Stderr, err)
		os.Exit(1)
	}
	if err := scr.Init(); err != nil {
		fmt.Fprintln(os.Stderr, err)
		os.Exit(1)
	}
	defer scr.Fini()
	scr.HideCursor()

	g := &game{scr: scr, rng: rand.New(rand.NewSource(time.Now().UnixNano())), hi: loadHi()}
	g.layout()
	g.reset()

	events := make(chan tcell.Event, 64)
	go func() {
		for {
			ev := scr.PollEvent()
			if ev == nil {
				return
			}
			events <- ev
		}
	}()

	tick := time.NewTicker(time.Second / frameRate)
	defer tick.Stop()
	last := time.Now()
	for {
		select {
		case ev := <-events:
			switch ev := ev.(type) {
			case *tcell.EventKey:
				if !g.key(ev) {
					return
				}
			case *tcell.EventResize:
				g.layout()
				scr.Sync()
			}
		case now := <-tick.C:
			dt := math.Min(now.Sub(last).Seconds(), 0.1)
			last = now
			g.update(dt)
			g.draw()
		}
	}
}

// ------------------------------------------------------------------- helpers

func abs(x int) int {
	if x < 0 {
		return -x
	}
	return x
}

func filter[T any](xs []T, keep func(T) bool) []T {
	out := xs[:0]
	for _, x := range xs {
		if keep(x) {
			out = append(out, x)
		}
	}
	for i := len(out); i < len(xs); i++ {
		var zero T
		xs[i] = zero
	}
	return out
}
