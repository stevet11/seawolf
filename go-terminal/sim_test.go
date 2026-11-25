package main

import (
	"math/rand"
	"testing"

	"github.com/gdamore/tcell/v2"
)

func TestSimulatedPlay(t *testing.T) {
	t.Setenv("HOME", t.TempDir())
	for _, sz := range [][2]int{{80, 24}, {60, 22}, {200, 60}, {30, 10}} {
		s := tcell.NewSimulationScreen("")
		if err := s.Init(); err != nil {
			t.Fatal(err)
		}
		s.SetSize(sz[0], sz[1])
		g := &game{scr: s, rng: rand.New(rand.NewSource(1))}
		g.layout()
		g.reset()
		g.state = stPlaying
		for i := 0; i < 30*200; i++ {
			if i%7 == 0 {
				g.fire()
			}
			g.px = 4 + (i/3)%(max(1, g.w-9))
			g.update(1.0 / 30)
			g.draw()
			if g.state == stOver {
				break
			}
		}
		if g.state != stOver {
			t.Errorf("%v: game never ended", sz)
		}
		t.Logf("%v: score=%d hits=%d shots=%d", sz, g.score, g.hits, g.shotsFired)
		s.Fini()
	}
}
