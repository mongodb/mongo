package examples

type Game struct {
	rolls   []int
	current int
}

func NewGame() *Game {
	game := new(Game)
	game.rolls = make([]int, maxThrowsPerGame)
	return game
}

func (self *Game) Roll(pins int) {
	self.rolls[self.current] = pins
	self.current++
}

func (self *Game) Score() (sum int) {
	for throw, frame := 0, 0; frame < framesPerGame; frame++ {
		if self.isStrike(throw) {
			sum += self.strikeBonusFor(throw)
			throw += 1
		} else if self.isSpare(throw) {
			sum += self.spareBonusFor(throw)
			throw += 2
		} else {
			sum += self.framePointsAt(throw)
			throw += 2
		}
	}
	return sum
}

func (self *Game) isStrike(throw int) bool {
	return self.rolls[throw] == allPins
}
func (self *Game) strikeBonusFor(throw int) int {
	return allPins + self.framePointsAt(throw+1)
}

func (self *Game) isSpare(throw int) bool {
	return self.framePointsAt(throw) == allPins
}
func (self *Game) spareBonusFor(throw int) int {
	return allPins + self.rolls[throw+2]
}

func (self *Game) framePointsAt(throw int) int {
	return self.rolls[throw] + self.rolls[throw+1]
}

const (
	allPins          = 10
	framesPerGame    = 10
	maxThrowsPerGame = 21
)
